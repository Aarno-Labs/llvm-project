//===--- RefoldStructuralGapCrossingProof.h --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-structure proof that a payload may cross one preserved preprocessing
// structure.
//
// Structural tiling splits a token hunk at a physical source gap that preserves
// preprocessing structure.  When the A/B alignment does not fix which side of
// that gap an undetermined B payload belongs on, the split is admissible only
// if committing the payload after the gap re-preprocesses to the same tokens.
// That is a question about what the payload can observe, and it is asked of
// each structure in the gap separately: a gap holding a `#define` and a
// `#warning` is answered by composing the two answers, not by one rule that
// recognizes the pair.
//
// The service is evidence-only.  It reads producer records and payload text and
// returns a typed answer per structure; it never edits, orders, or selects.
// Every structure kind with no rule here -- an unclassified pragma, an
// unrecognized directive, `#import` -- reports unproven, which keeps the caller
// failing closed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALGAPCROSSINGPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALGAPCROSSINGPROOF_H

#include "macro/RefoldMacroStateProof.h"
#include "source/RefoldPreprocessingStructureIndex.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace clang {
namespace refold {

class RefoldModel;
class RefoldTokenTextAnalysis;

/// The reason one preserved structure does not fix the payload's side.
///
/// Each value names the theorem that discharged the crossing, so a trace can
/// report *why* a gap was crossable rather than only that it was.
enum class GapCrossingProofKind : uint8_t {
  /// A `#define`/`#undef` whose bound name the committed payload cannot
  /// observe, in that binding's own observation mode.
  MacroStateDirectiveUnobserved,

  /// A pragma the preprocessor consumes, whose state effect the payload cannot
  /// observe.  Both the `#pragma` and `_Pragma` spellings reach this rule.
  ConsumedPragmaUnobserved,

  /// A `#warning`, or a `#error` the producer recorded inside a conditional arm
  /// it did not select.  Neither mutates preprocessor state nor contributes a
  /// token, so no payload can observe either one.
  StatelessDiagnosticDirective,

  /// A conditional-control directive whose following text the producer recorded
  /// as selected, so the payload's new home is reached exactly as its old one
  /// was.
  ConditionalControlSelectedArm,

  /// An `#include` instance the producer recorded as contributing no token and
  /// introducing no preprocessing state at all.
  StatelessIncludeInstance,

  /// A `#line` whose logical-position state the committed payload contains no
  /// observer of, directly or through a live macro's replacement list.
  LineControlUnobserved,
};

/// Return a stable diagnostic spelling for a crossing proof kind.
llvm::StringRef toString(GapCrossingProofKind kind);

/// Why a preserved structure was left fixing the payload's side.
enum class GapCrossingRejection : uint8_t {
  /// The crossing was proven; no rejection applies.
  None,
  /// No rule answers this structure kind, so it defaults to observable.
  NoRuleForStructureKind,
  /// The structure has no unique producer record, so its content is unknown.
  NoProducerRecord,
  /// The payload can itself introduce a preprocessing directive, so its
  /// placement is not merely an observation question.
  PayloadCarriesDirective,
  /// The payload observes the state this structure changes.
  PayloadObservesState,
  /// A pragma spelling the taxonomy does not classify, or one the preprocessor
  /// re-emits into both streams so the payload's side is token order.
  PragmaNotConsumed,
  /// The producer recorded state under an include instance, so crossing it is
  /// not a no-op.
  IncludeInstanceCarriesState,
  /// The producer did not record the payload's new home as selected.
  ArmNotSelected,
};

/// Return a stable diagnostic spelling for a crossing rejection.
llvm::StringRef toString(GapCrossingRejection rejection);

/// Typed answer for one preserved structure in a gap.
struct GapCrossingEvidence {
  /// Kind of the structure this evidence is about.
  PreprocessingStructureKind structureKind =
      PreprocessingStructureKind::OtherDirective;
  /// Half-open physical byte range of the structure, in its own source file.
  uint64_t structureBegin = 0;
  uint64_t structureEnd = 0;
  /// Set exactly when the crossing is proven.
  std::optional<GapCrossingProofKind> proof;
  /// Set exactly when `proof` is absent.
  GapCrossingRejection rejection = GapCrossingRejection::NoRuleForStructureKind;

  bool Proven() const { return proof.has_value(); }
};

/// Composed answer for one complete structural gap.
struct GapCrossingProof {
  /// One entry per queried structure, in source order.
  llvm::SmallVector<GapCrossingEvidence, 4> structures;

  /// Macro-state bindings recovered from the gap's `#define`/`#undef`
  /// intervals.  The caller needs these to decide whether the macro-state
  /// liveness planner owns the seam instead.
  llvm::SmallVector<MacroStateBinding, 2> macroBindings;

  /// False when a `#define`/`#undef` in the gap had no recoverable producer
  /// record, so `macroBindings` is not the complete set and no rule may
  /// conclude anything from a name's absence.
  bool macroBindingsComplete = true;

  /// True when the gap preserves at least one `#define`/`#undef`.
  bool carriesMacroStateDirective = false;

  /// Return whether every structure in a non-empty gap is crossable.
  ///
  /// An empty structure list is not crossable.  A run boundary exists only
  /// because something was preserved there, so an empty list means the
  /// structures were never recorded, and reporting true would let a placement
  /// theorem run with nothing to reason about.
  bool Crossable() const {
    if (structures.empty())
      return false;
    for (const GapCrossingEvidence &evidence : structures)
      if (!evidence.Proven())
        return false;
    return true;
  }
};

/// Read-only per-structure gap-crossing proof service.
///
/// Inputs are immutable producer facts and payload text.  The service holds no
/// state between queries and never consults source bytes: every question it
/// answers is decided from a producer record, which is what keeps the answers
/// correct for a gap inside an included header as well as for a TU gap.
class RefoldStructuralGapCrossingProver {
public:
  /// Borrowed services required to read producer records and payload text.
  struct Dependencies {
    /// Producer model holding the directive, include, pragma, line-control and
    /// conditional records bound to the queried intervals.
    const RefoldModel &model;
    /// Macro-state proof service deciding what a payload can observe.
    const RefoldMacroStateProof &macroStateProof;
    /// Raw-token text analysis used for the payload's directive inventory.
    const RefoldTokenTextAnalysis &tokenText;
    /// Language mode used while lexing payload text.
    const clang::LangOptions &lexLang;
  };

  /// One placement question about one structural gap.
  struct Query {
    /// Protected intervals preserved in the gap, in source order.
    llvm::ArrayRef<const PreprocessingStructureInterval *> structures;

    /// B-derived bytes whose side of the gap the alignment does not fix.
    llvm::StringRef payload;

    /// B-derived bytes from the committed boundary to the end of the hunk.
    ///
    /// Committing the lower frontier emits all of these after the gap, so this
    /// is the text every observation question is asked of.  It is a superset of
    /// `payload`: the tokens outside `payload` have a forced alignment and were
    /// always going to land here, so asking about them is deliberately
    /// conservative rather than necessary.
    llvm::StringRef committedSuffix;

    /// Conditional arm owning the run the payload is committed to, or nullopt
    /// when that run is not inside any conditional arm.
    std::optional<uint64_t> committedArmId;
  };

  explicit RefoldStructuralGapCrossingProver(Dependencies deps) : deps_(deps) {}

  /// Answer the placement question for every structure in one gap.
  GapCrossingProof Prove(const Query &query) const;

private:
  /// Answer one structure, without the macro-state directives.
  ///
  /// `#define`/`#undef` are answered together by `Prove()` because the
  /// macro-state proof takes the complete binding set: an identifier in the
  /// payload may expand to a name bound by a different directive in the same
  /// gap, and only the whole set makes that reachable-set question exact.
  GapCrossingEvidence
  ProveOneStructure(const PreprocessingStructureInterval &interval,
                    const Query &query) const;

  /// Answer a consumed-pragma structure from its producer record.
  GapCrossingEvidence
  ProvePragma(const PreprocessingStructureInterval &interval,
              const Query &query) const;

  /// Answer a conditional-control structure from the committed run's arm.
  GapCrossingEvidence
  ProveConditionalControl(const PreprocessingStructureInterval &interval,
                          const Query &query) const;

  /// Answer an `#include`/`#include_next` structure from its instance records.
  GapCrossingEvidence
  ProveInclude(const PreprocessingStructureInterval &interval,
               const Query &query) const;

  /// Answer a `#line` structure from the committed payload's observers.
  GapCrossingEvidence
  ProveLineControl(const PreprocessingStructureInterval &interval,
                   const Query &query) const;

  /// Answer a `#warning`/`#error` structure.
  GapCrossingEvidence
  ProveDiagnosticDirective(const PreprocessingStructureInterval &interval,
                           const Query &query) const;

  /// Return whether the committed text can introduce a preprocessing directive.
  ///
  /// A payload that can is not answered by any observation rule here: moving it
  /// across a structure could change what that structure does, not merely what
  /// the payload sees.  B is a preprocessed stream, so this is reachable in
  /// practice only through a pragma the preprocessor re-emitted into it.
  bool CommittedTextCarriesDirective(const Query &query) const;

  /// Return the unique producer pragma record bound to `interval`.
  const RefoldModel::PragmaDirective *
  ProducerPragmaFor(const PreprocessingStructureInterval &interval) const;

  /// Return the unique producer line-control record bound to `interval`.
  const RefoldModel::LineControlEvent *
  ProducerLineControlFor(const PreprocessingStructureInterval &interval) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALGAPCROSSINGPROOF_H
