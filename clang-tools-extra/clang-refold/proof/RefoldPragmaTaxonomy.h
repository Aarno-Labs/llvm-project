//===--- RefoldPragmaTaxonomy.h - Pragma classification ---------*- C++ -*-===//
//
// One classification of pragma spellings, answering the two questions the
// engine asks about a pragma it cannot delete.
//
//   * What state does it change?  A payload moved across the directive is sound
//     only when re-preprocessing that payload cannot observe the change.
//   * Does it bind to the construct that follows it?  Replacing or deleting
//     that construct is wrong even when no directive moved and none was
//     crossed: `#pragma omp parallel for` means nothing without its loop.
//
// These were previously two separate deferrals in the terminal-fallback
// roadmap, described as unrelated.  They are one artifact seen from two sides:
// the same vocabulary, the same operands, and the same rule for spellings
// nobody classified.  Splitting them is how two classifications of the same
// pragma drift apart and disagree.
//
// Every unclassified spelling is `Unknown`, which is simultaneously the most
// state-changing and the most binding answer.  Adding a spelling here can only
// admit realizations that were previously refused; it can never remove a
// restriction from a spelling that is already classified.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMATAXONOMY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMATAXONOMY_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace clang {
namespace refold {

/// Preprocessor or compiler state a pragma directive changes.
///
/// The values name state *kinds*, not spellings: two spellings with the same
/// observable effect share a value, and a spelling whose effect the engine does
/// not model stays `Unknown`.
enum class PragmaStateEffect : uint8_t {
  /// Unclassified spelling.  Assumed to change arbitrary state and to bind to
  /// whatever follows it.  This is the only safe default: a vendor pragma the
  /// engine has never seen may do either.
  Unknown,

  /// Diagnostic output only, with no effect on preprocessing or codegen:
  /// `message`, `warning`, `error`.
  NoState,

  /// Suppresses later textual inclusion of the containing file: `once`.
  IncludeOnce,

  /// Makes identifiers illegal for the rest of the translation unit:
  /// `GCC poison`.  The identifiers are recorded in `namedIdentifiers`.
  PoisonIdentifiers,

  /// Saves or restores one macro definition: `push_macro`, `pop_macro`.  The
  /// macro name is recorded in `namedIdentifiers`.
  MacroStateStack,

  /// Diagnostic severity state: `clang diagnostic`, `GCC diagnostic`.  Affects
  /// which diagnostics fire, never which tokens are produced.
  DiagnosticState,

  /// Marks the containing file as a system header, suppressing its warnings:
  /// `GCC system_header`.
  SystemHeader,
};

/// Whether a pragma's meaning attaches to the construct that follows it.
enum class PragmaConstructBinding : uint8_t {
  /// The pragma modifies the next construct, so replacing or deleting that
  /// construct changes what the pragma means.  `omp parallel for`,
  /// `clang loop`, `GCC ivdep` and every unclassified spelling are here.
  BindsFollowingConstruct,

  /// The pragma's effect is independent of what follows it syntactically.
  NonBinding,
};

/// Result of classifying one pragma spelling.
///
/// `namedIdentifiers` points into the caller's text and must not outlive it.
struct PragmaClassification {
  PragmaStateEffect effect = PragmaStateEffect::Unknown;
  PragmaConstructBinding binding =
      PragmaConstructBinding::BindsFollowingConstruct;

  /// Identifiers the directive names, in source order: the poisoned
  /// identifiers for `GCC poison`, or the single macro name for
  /// `push_macro`/`pop_macro`.  Empty for every other effect.
  llvm::SmallVector<llvm::StringRef, 4> namedIdentifiers;

  /// Return whether the spelling was recognized.  An unrecognized spelling is
  /// not an error; it is the fail-closed answer.
  bool IsClassified() const { return effect != PragmaStateEffect::Unknown; }
};

/// Classify one complete `#pragma` directive line.
///
/// `directiveText` is the full logical directive, with or without a trailing
/// newline.  Trailing comments are directive trivia and do not prevent
/// recognition; an incomplete comment does, because the spelling is then not
/// provably complete.  Anything unrecognized returns `Unknown`, which callers
/// must treat as both state-changing and binding.
PragmaClassification classifyPragmaDirective(llvm::StringRef directiveText);

/// Return whether a bare pragma operand sequence names `once`.
///
/// `_Pragma("once")` carries its operand without the `#pragma` introducer, so
/// the operator detector needs the operand grammar on its own.  Sharing it with
/// `classifyPragmaDirective` keeps one definition of what counts as once-state.
bool pragmaOperandNamesOnce(llvm::StringRef operandText);

/// A caller's proof about the macro definitions a classification binds.
///
/// `payloadObservesPragmaState` cannot answer that question.  Deciding whether
/// re-preprocessing a payload can observe a change to one macro's definition
/// needs the producer's macro records -- which identifiers are bound anywhere
/// in the run, and in what shape -- and this classification is deliberately a
/// pure function of text.  Handing it the proof *service* would end that; so a
/// caller holding `RefoldMacroStateProof` answers the question itself and
/// passes the verdict here as a fact.
///
/// Every caller without such a proof leaves this `Unproven`, which keeps the
/// historical rule: a `MacroStateStack` payload containing any identifier is
/// observing.
enum class MacroStateObservationAnswer : uint8_t {
  /// Not answered; `MacroStateStack` falls back to the any-identifier rule.
  Unproven,
  /// Proven that re-preprocessing the payload cannot observe the change.
  Unobserved,
  /// The caller's proof reports the change observable.
  Observed,
};

/// Return whether re-preprocessing `payload` on the other side of this pragma
/// could observe the state it changes.
///
/// This is the admissibility question for moving a payload across a preserved
/// directive, or equivalently for choosing which side of it an unaligned
/// payload belongs on.  `true` means the placement is observable and must be
/// determined by other means; `false` means both placements are equivalent.
///
/// The rules are per effect kind:
///
///   * `Unknown` observes everything.
///   * `NoState`, `DiagnosticState` and `SystemHeader` observe nothing: they
///     change which diagnostics fire, never which tokens are produced.
///   * `IncludeOnce` is observed only by a payload that can perform inclusion,
///     so a payload containing no directive-introducing token is unaffected.
///   * `PoisonIdentifiers` is observed by a payload naming a poisoned
///     identifier.
///   * `MacroStateStack` is answered by `macroState` when the caller proved it,
///     and otherwise by the fail-closed rule that any payload containing an
///     identifier observes.
///
/// `macroState` applies only to `MacroStateStack`.  It is ignored for every
/// other effect, whose observation is a property of the payload's own text.
bool payloadObservesPragmaState(
    const PragmaClassification &classification, llvm::StringRef payload,
    const LangOptions &lang,
    MacroStateObservationAnswer macroState =
        MacroStateObservationAnswer::Unproven);

/// Return a stable diagnostic spelling for a state effect.
llvm::StringRef toString(PragmaStateEffect effect);

/// Return a stable diagnostic spelling for a construct binding.
llvm::StringRef toString(PragmaConstructBinding binding);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMATAXONOMY_H
