//===--- RefoldProofVocabulary.h -------------------------------*- C++ -*-===//
//
// Shared proof vocabulary for clang-refold.
//
// Consolidates the value-only proof carriers that proof modules
// exchange:
//   - producer-include spelling helpers and small carrier records,
//   - theorem-facing witness, resolver, and audit value records, and
//   - terminal-fallback proof vocabulary used by owner-state and
//     accepted-result carriers.
//
// `ProofDischargeAccumulator` lives in `RefoldAcceptedResultTypes.h`
// because it operates directly on the `ProofDischargeRecord` defined
// there; placing it here would create a header cycle
// (vocabulary ⇄ accepted-result types).  The mutable terminal request
// sink and the theorem-audit service live in
// `proof/RefoldTerminalProofSink.{h,cpp}` and
// `proof/RefoldTheoremAudit.{h,cpp}`; only the data-only terminal
// carriers belong here.
//
// Everything here is intentionally value-only.  Mutable proof state,
// edit emission, fallback requests, materialization recursion, and
// engine caches belong in their respective service modules; this
// header may not acquire any of those.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFVOCABULARY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFVOCABULARY_H

#include "core/RefoldModel.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

/// Opaque forward declaration of the accepted-path discriminator.  The full
/// definition lives in RefoldAcceptedResultTypes.h, which includes this header,
/// so it cannot be included back here without a cycle.  A fixed-underlying-type
/// enum can be named by value, which is all `RefoldWitness` needs to carry the
/// path kind as a typed discriminator alongside its trace-only spelling.
enum class AcceptedPathKind : uint8_t;

//===----------------------------------------------------------------------===//
// Producer-include spelling helpers and small proof carriers.
//===----------------------------------------------------------------------===//

/// Return the explicit producer-entered filename spelling for an include edge.
///
/// New-schema maps carry this exact `__FILE__` entry spelling in
/// entered_file_spelling.  This helper intentionally does not fall back to
/// resolved_path: file-spelling proofs need to distinguish an explicit producer
/// fact from legacy spelling data and from independently recovered observer or
/// replay witnesses.
inline llvm::StringRef
explicitProducerEnteredFileSpelling(const RefoldModel::IncludeItem &include) {
  return include.enteredFileSpelling ? *include.enteredFileSpelling
                                     : llvm::StringRef();
}

/// Return the legacy spelling-oriented include path, if present.
///
/// Older maps only had resolved_path, whose meaning drifted between physical
/// identity and entered-file spelling.  New proof code should consult this
/// helper only after exhausting stronger new-schema metadata and recovered
/// observer/replay witnesses.
inline llvm::StringRef
legacyResolvedIncludePath(const RefoldModel::IncludeItem &include) {
  return include.resolvedPath ? *include.resolvedPath : llvm::StringRef();
}

/// Return the best available producer-entered filename spelling for an include.
///
/// This is a convenience for legacy-neutral callers that only need a spelling
/// anchor.  Proof-sensitive code that implements the full file-spelling
/// fallback hierarchy should prefer explicitProducerEnteredFileSpelling(), then
/// any context-specific observer/replay witnesses, and only then
/// legacyResolvedIncludePath().
inline llvm::StringRef
producerEnteredFileSpelling(const RefoldModel::IncludeItem &include) {
  llvm::StringRef explicitSpelling =
      explicitProducerEnteredFileSpelling(include);
  return !explicitSpelling.empty() ? explicitSpelling
                                   : legacyResolvedIncludePath(include);
}

/// Return the exact producer `__FILE_NAME__` entry spelling when available.
///
/// entered_file_name is emitted by the producer using Clang's own
/// processPathToFileName() logic.  If an old/new map lacks it, fall back to the
/// deterministic refolder basename helper over entered_file_spelling / legacy
/// resolved_path.  This fallback is compatibility-only; new maps should carry
/// entered_file_name whenever the include was actually entered.
inline llvm::StringRef
producerEnteredFileName(const RefoldModel::IncludeItem &include) {
  if (include.enteredFileName)
    return *include.enteredFileName;
  llvm::StringRef fileSpelling = producerEnteredFileSpelling(include);
  return fileSpelling.empty() ? llvm::StringRef()
                              : stringutils::pathBasename(fileSpelling);
}

/// Return the producer-side path spelling used as input to physical identity.
///
/// New maps carry opened_path for physical/FileEntry identity.  Legacy maps
/// fall back to resolved_path, and callers must canonicalize only inside the
/// physical proof path, e.g. through RefoldPathIdentity::PathsEqual().  Never
/// use entered_file_spelling here: observer spelling and filesystem identity
/// are intentionally separate proof domains.
inline std::optional<std::filesystem::path>
producerPhysicalIncludePath(const RefoldModel::IncludeItem &include) {
  llvm::StringRef path = include.openedPath
                             ? *include.openedPath
                             : legacyResolvedIncludePath(include);
  if (path.empty())
    return std::nullopt;
  return std::filesystem::path(path.str());
}

/// Proof-audit mode for the witness resolver.
///
/// Default derives from normal refolding mode: strict refolding enables
/// authoritative proof audit, while non-strict refolding leaves the resolver
/// off unless the caller explicitly requests probe or strict audit.
enum class ProofAuditMode : uint8_t { Default, Off, Probe, Strict };

/// Final source location used by include replay proofs.
///
/// This models the source file that the driver will write to `--out` and
/// subsequently pass to `--check`.  Direct quoted include lookup starts from
/// this directory.  It must not silently fall back to the producer TU path: the
/// producer source location and the emitted `.c.mod` location can differ, and
/// proving an operand from the wrong directory is precisely the class of
/// relocation bug this surface prevents.
struct FinalReplaySurface {
  std::filesystem::path outputPath;
  std::filesystem::path outputDirectory;
  std::filesystem::path originalWorkingDirectory;
  std::string outputDirectorySpelling;
};

/// Describes which components of the logical location are observed by preserved
/// location-sensitive builtins in an owner suffix.
///
/// Proof modules exchange this demand through service callbacks; the queries
/// that compute it remain on the engine-facing line-control proof surface.
struct LineStateObserverDemand {
  bool needsLine = false;
  bool needsFile = false;
  bool needsFileName = false;

  // True when at least one demand witness needs model metadata rather than
  // direct lexical final-source spelling.  This is construction metadata for
  // producing a repair obligation, not a late pruning veto.
  bool hasModelBackedLineStateDemand = false;

  bool Any() const { return needsLine || needsFile || needsFileName; }
  bool PrunableByCompactFinalLineControl() const { return Any(); }
};

//===----------------------------------------------------------------------===//
// Theorem-facing witness, resolver, and audit value records.
//===----------------------------------------------------------------------===//

/// \brief Vocabulary for emission paths that still require legacy-authority
/// audit.
///
/// A path is legacy only when emitted behavior is justified by one of these
/// implementation-local mechanisms instead of by the proof lattice.  This is
/// intentionally a definition layer, not an audit: no caller is rejected
/// merely by naming a kind here.  Audit sites use this shared vocabulary so
/// every diagnostic answers the same question: which implementation-local
/// authority must be represented by which proof-lattice invariant before it is
/// allowed to survive to selection or emission?
#define REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)                                 \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PathSpecificProofMirror)                                            \
  REFOLD_X(PostSummaryProofKindDecision)                                       \
  REFOLD_X(StructurePreservingProofBit)                                        \
  REFOLD_X(UnclassifiedFallbackBranch)                                         \
  REFOLD_X(StateCheckOutsideGateway)                                           \
  REFOLD_X(FinalLineControlLivenessWithoutObligation)

enum class LegacyPathKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(LegacyPathKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case LegacyPathKind::name:                                                   \
    return #name;
    REFOLD_LEGACY_PATH_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}

/// \brief Canonical definition for one legacy-path category.
///
/// `definition` states the forbidden legacy dependency.  `requiredClosure`
/// names the proof-lattice representation that must replace that dependency
/// before the path can be theorem-facing.  Keeping the prose centralized keeps
/// audit output, comments, and future CI checks synchronized instead of
/// allowing six slightly different definitions of "legacy path" to drift.
struct LegacyPathDefinition {
  LegacyPathKind kind = LegacyPathKind::Unknown;
  StringRef definition;
  StringRef requiredClosure;
};

inline LegacyPathDefinition describeLegacyPathKind(LegacyPathKind kind) {
  switch (kind) {
  case LegacyPathKind::Unknown:
    return {kind, "unclassified legacy dependency",
            "classify the dependency before it can be audited"};
  case LegacyPathKind::PathSpecificProofMirror:
    return {kind,
            "path-specific proof booleans or witness flags mirror "
            "ProofSummary",
            "move the fact into ProofSummary / the canonical emitted proof "
            "carrier"};
  case LegacyPathKind::PostSummaryProofKindDecision:
    return {kind,
            "proofKind or accepted-path provenance decides behavior after "
            "ProofSummary / TheoremProofClass should dominate",
            "normalize to exactly one TheoremProofClass before selection or "
            "emission"};
  case LegacyPathKind::StructurePreservingProofBit:
    return {kind,
            "structurePreserving is treated as proof authority rather than "
            "construction metadata",
            "represent preservation with an explicit theorem proof class, "
            "typed witness, and lattice preference"};
  case LegacyPathKind::UnclassifiedFallbackBranch:
    return {kind,
            "fallback behavior is not represented by a classified "
            "TerminalFallbackProofFailure",
            "build a TerminalFallbackProofFailure / TerminalFallbackWitness "
            "or convert the path to an in-domain lattice proof"};
  case LegacyPathKind::StateCheckOutsideGateway:
    return {kind,
            "preprocessor-state acceptance is checked outside "
            "OwnerStateDelta, OwnerStateGraph, or the state-transition "
            "gateway",
            "route the state fact through OwnerStateDelta / OwnerStateGraph "
            "/ gateway witnesses"};
  case LegacyPathKind::FinalLineControlLivenessWithoutObligation:
    return {kind,
            "final #line liveness is decided without a compact line-control "
            "obligation or removal proof",
            "represent liveness as a typed final-line-control obligation / "
            "proof before replacing the operational scanner"};
  }
  return {LegacyPathKind::Unknown, "unclassified legacy dependency",
          "classify the dependency before it can be audited"};
}

inline bool isDefinedLegacyPathKind(LegacyPathKind kind) {
  return kind != LegacyPathKind::Unknown;
}

/// \brief One semantic no-legacy audit finding.
///
/// The audit names the legacy category, the engine boundary that observed it,
/// and a deterministic detail string.  During strict/theorem validation it
/// becomes theorem state and may force the same explicit terminal fallback as
/// other audit violations.
struct LegacyAuditEvidence {
  LegacyPathKind kind = LegacyPathKind::Unknown;
  std::string role;
  std::string detail;
};
#undef REFOLD_LEGACY_PATH_KIND_LIST

/// \brief Trace-only vocabulary for the global witness-resolution model.
///
/// These names do not select or reject output.  They provide stable, theorem-
/// facing terminology for proof families that move each proof family from ad
/// hoc candidate choice to equivalence-class canonical witness resolution.
/// Unknown fields are intentionally explicit: a trace entry may be partial
/// during this proof pass, but it must not pretend that an uncomputed proof
/// dimension is equivalent to anything else.
#define REFOLD_WITNESS_PROOF_FAMILY_LIST(REFOLD_X)                             \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(AcceptedResult)                                                     \
  REFOLD_X(MacroActualRepair)                                                  \
  REFOLD_X(DefinitionTapeReplay)                                               \
  REFOLD_X(GeneratedCalleeReplay)                                              \
  REFOLD_X(Stringification)                                                    \
  REFOLD_X(TokenPaste)                                                         \
  REFOLD_X(VariadicComma)                                                      \
  REFOLD_X(ZeroTokenBoundary)                                                  \
  REFOLD_X(LineControlObserver)                                                \
  REFOLD_X(CounterState)                                                       \
  REFOLD_X(IncludePreservation)                                                \
  REFOLD_X(IncludeRealization)                                                 \
  REFOLD_X(TUAnchor)                                                           \
  REFOLD_X(TUTextEdit)                                                         \
  REFOLD_X(OwnerRealization)                                                   \
  REFOLD_X(MixedOwnerTiling)                                                   \
  REFOLD_X(TerminalFallback)

enum class WitnessProofFamily : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_PROOF_FAMILY_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessProofFamily value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessProofFamily::name:                                               \
    return #name;
    REFOLD_WITNESS_PROOF_FAMILY_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_PROOF_FAMILY_LIST

#define REFOLD_WITNESS_PRODUCER_KIND_LIST(REFOLD_X)                            \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(Forward)                                                            \
  REFOLD_X(Stringify)                                                          \
  REFOLD_X(PasteLeft)                                                          \
  REFOLD_X(PasteRight)                                                         \
  REFOLD_X(PasteResult)                                                        \
  REFOLD_X(VariadicForward)                                                    \
  REFOLD_X(ZeroTokenAnchor)                                                    \
  REFOLD_X(VariadicMissing)                                                    \
  REFOLD_X(VariadicEmpty)                                                      \
  REFOLD_X(VariadicCommaInsertion)                                             \
  REFOLD_X(VariadicCommaElision)                                               \
  REFOLD_X(VaOptActivation)                                                    \
  REFOLD_X(VaOptErasure)                                                       \
  REFOLD_X(GeneratedCallee)                                                    \
  REFOLD_X(ObjectAlias)                                                        \
  REFOLD_X(DirectiveMaterialization)                                           \
  REFOLD_X(BuiltinMaterialization)                                             \
  REFOLD_X(CounterConsumption)                                                 \
  REFOLD_X(OwnerRealization)                                                   \
  REFOLD_X(TerminalMaterialization)

enum class WitnessProducerKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_PRODUCER_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessProducerKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessProducerKind::name:                                              \
    return #name;
    REFOLD_WITNESS_PRODUCER_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_PRODUCER_KIND_LIST

#define REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(RootInvocation)                                                     \
  REFOLD_X(NestedInvocation)                                                   \
  REFOLD_X(MacroArgument)                                                      \
  REFOLD_X(IncludeBoundary)                                                    \
  REFOLD_X(TUAnchorBoundary)                                                   \
  REFOLD_X(ZeroTokenBoundary)                                                  \
  REFOLD_X(OwnerRealizationBoundary)                                           \
  REFOLD_X(TerminalBoundary)

enum class WitnessBoundaryClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessBoundaryClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessBoundaryClass::name:                                             \
    return #name;
    REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_BOUNDARY_CLASS_LIST

#define REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)                         \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PreservesDiagnostics)                                               \
  REFOLD_X(RealizesEditedSurface)                                              \
  REFOLD_X(TerminalOutOfDomain)

enum class WitnessDiagnosticClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessDiagnosticClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessDiagnosticClass::name:                                           \
    return #name;
    REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST

#define REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)                        \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(LocalOnly)                                                          \
  REFOLD_X(OwnerClosed)                                                        \
  REFOLD_X(MixedOwnerTile)                                                     \
  REFOLD_X(Terminal)

enum class WitnessCompositionClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessCompositionClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessCompositionClass::name:                                          \
    return #name;
    REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_COMPOSITION_CLASS_LIST

#define REFOLD_WITNESS_REJECT_REASON_LIST(REFOLD_X)                            \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(NotSelectable)                                                      \
  REFOLD_X(ProofNormalizationFailed)                                           \
  REFOLD_X(MissingEquivalenceKey)                                              \
  REFOLD_X(ProducerKindMismatch)                                               \
  REFOLD_X(StateUnknown)                                                       \
  REFOLD_X(NonEquivalentAmbiguity)                                             \
  REFOLD_X(SelectorOnlyNoEmittedCandidate)                                     \
  REFOLD_X(TerminalFallbackRequested)

enum class WitnessRejectReason : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_REJECT_REASON_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessRejectReason value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessRejectReason::name:                                              \
    return #name;
    REFOLD_WITNESS_REJECT_REASON_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_REJECT_REASON_LIST

#define REFOLD_WITNESS_FALLBACK_CLASS_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(NoOwnerClosedWitness)                                               \
  REFOLD_X(MultipleNonEquivalentWitnessClasses)                                \
  REFOLD_X(UnknownTargetPreprocessedTokens)                                    \
  REFOLD_X(UnknownSuffixState)                                                 \
  REFOLD_X(UnprovenProducerKind)                                               \
  REFOLD_X(CounterStateMismatch)                                               \
  REFOLD_X(LineControlObserverMismatch)                                        \
  REFOLD_X(InvalidMacroInvocation)                                             \
  REFOLD_X(InvalidPasteResult)                                                 \
  REFOLD_X(UnsupportedDirectiveInteraction)                                    \
  REFOLD_X(CompositionFailure)                                                 \
  REFOLD_X(ValidationFailure)                                                  \
  REFOLD_X(UnconvertedProofFamily)                                             \
  REFOLD_X(IncompleteWitnessKey)                                               \
  REFOLD_X(NoSelectableWitness)

/// \brief Normalized strict-domain fallback class for the current proof
/// model.
///
/// Terminal raw-B fallback and resolver fallback are separate implementation
/// paths, but the theorem only cares which proof obligation failed.  This
/// vocabulary is intentionally small and stable so traces can distinguish an
/// unavoidable out-of-domain fallback from an avoidable missing proof family.
enum class WitnessFallbackClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_FALLBACK_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessFallbackClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessFallbackClass::name:                                             \
    return #name;
    REFOLD_WITNESS_FALLBACK_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_FALLBACK_CLASS_LIST

#define REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST(REFOLD_X)                      \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DeclaredInDomain)                                                   \
  REFOLD_X(PotentiallyInDomainMissingProof)                                    \
  REFOLD_X(ExplicitOutOfDomain)                                                \
  REFOLD_X(AmbiguousOutOfDomain)

/// \brief Position of a witness decision relative to the strict in-domain
/// fragment declared for the strict-domain resolver.
///
/// `PotentiallyInDomainMissingProof` is intentionally distinct from
/// `ExplicitOutOfDomain`: it means the active proof lattice lacks a strong
/// enough proof family or key dimension to claim the theorem, not that the
/// source edit is mathematically outside the declared fragment.
enum class WitnessStrictDomainClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessStrictDomainClass value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessStrictDomainClass::name:                                         \
    return #name;
    REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST

#define REFOLD_WITNESS_STRICT_DOMAIN_OBLIGATION_LIST(REFOLD_X)                 \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(FiniteDeterministicTiling)                                          \
  REFOLD_X(ProducerProvenSourceWitness)                                        \
  REFOLD_X(OwnerClosure)                                                       \
  REFOLD_X(TargetPreprocessedTokens)                                           \
  REFOLD_X(StateEquivalence)                                                   \
  REFOLD_X(LineControlObserverEquivalence)                                     \
  REFOLD_X(CounterEquivalence)                                                 \
  REFOLD_X(ModeledProducerSemantics)                                           \
  REFOLD_X(ValidSourceRepair)                                                  \
  REFOLD_X(Composition)                                                        \
  REFOLD_X(ConvertedProofFamily)                                               \
  REFOLD_X(CompleteWitnessKey)                                                 \
  REFOLD_X(FinalValidation)

/// \brief Strict-domain theorem obligation used for the current proof model
/// audit traces.
///
/// This is the compact, cross-family vocabulary for the declared fragment:
/// finite tiling, producer-proven witnesses, owner closure, known target
/// tokens, stable preprocessing state/observers/counters, modeled producer
/// semantics, valid source spelling, compositionality, converted proof
/// family coverage, complete keys, and final validation.
enum class WitnessStrictDomainObligation : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_STRICT_DOMAIN_OBLIGATION_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessStrictDomainObligation value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessStrictDomainObligation::name:                                    \
    return #name;
    REFOLD_WITNESS_STRICT_DOMAIN_OBLIGATION_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_STRICT_DOMAIN_OBLIGATION_LIST

/// \brief strict-domain classification for one resolver/fallback.
///
/// The decision separates three theorem-relevant outcomes:
///   - accepted inside the declared strict domain;
///   - plausibly in-domain but blocked by a missing/incomplete proof family;
///   - explicitly outside the domain or ambiguous across non-equivalent
///     repairs, where guessing author intent would violate the policy.
struct WitnessStrictDomainDecision {
  WitnessStrictDomainClass domainClass = WitnessStrictDomainClass::Unknown;
  WitnessStrictDomainObligation obligation =
      WitnessStrictDomainObligation::Unknown;
  WitnessFallbackClass fallbackClass = WitnessFallbackClass::Unknown;
  std::string reason;

  bool IsDeclaredInDomain() const {
    return domainClass == WitnessStrictDomainClass::DeclaredInDomain;
  }
};

/// \brief One semantic dimension inside a witness equivalence key.
///
/// Known/unknown status is explicit for every dimension that will participate
/// in equivalence-class partitioning.  An unknown dimension is never
/// equivalent to another unknown dimension merely because both spell
/// "unknown"; partition keys salt unknown dimensions with the witness id
/// until a later proof path supplies a real semantic value.
struct WitnessEquivalenceDimension {
  bool known = false;
  std::string value;
  std::string unknownReason = "not-carried";

  static WitnessEquivalenceDimension Known(llvm::StringRef value) {
    WitnessEquivalenceDimension dimension;
    dimension.known = true;
    dimension.value = value.str();
    dimension.unknownReason.clear();
    return dimension;
  }

  static WitnessEquivalenceDimension Unknown(llvm::StringRef reason) {
    WitnessEquivalenceDimension dimension;
    dimension.known = false;
    dimension.unknownReason = reason.empty() ? "not-carried" : reason.str();
    return dimension;
  }

  std::string ToString() const {
    if (known)
      return value;
    return llvm::formatv("unknown(reason={0})", unknownReason).str();
  }

  std::string PartitionValue(llvm::StringRef name, uint64_t witnessId) const {
    if (known)
      return value;
    return llvm::formatv("unknown:{0}:witness#{1}:{2}", name, witnessId,
                         unknownReason)
        .str();
  }
};

/// \brief Set-valued producer dimension for witness equivalence.
///
/// The first implementation usually carries a singleton, but the type is
/// intentionally a set so generated-callee, stringify, paste, variadic, and
/// directive materialization paths can add producer obligations without
/// replacing the equivalence vocabulary again.
struct WitnessProducerKindSet {
  bool known = false;
  std::vector<WitnessProducerKind> kinds;
  std::string unknownReason = "not-carried";

  static WitnessProducerKindSet Unknown(llvm::StringRef reason) {
    WitnessProducerKindSet set;
    set.known = false;
    set.unknownReason = reason.empty() ? "not-carried" : reason.str();
    return set;
  }

  static WitnessProducerKindSet KnownSingle(WitnessProducerKind kind) {
    WitnessProducerKindSet set;
    if (kind == WitnessProducerKind::Unknown) {
      set.known = false;
      set.unknownReason = "producer-kind-unknown";
      return set;
    }
    set.known = true;
    set.kinds.push_back(kind);
    return set;
  }

  void Add(WitnessProducerKind kind) {
    if (kind == WitnessProducerKind::Unknown)
      return;
    known = true;
    for (WitnessProducerKind existing : kinds)
      if (existing == kind)
        return;
    kinds.push_back(kind);
    std::sort(kinds.begin(), kinds.end(),
              [](WitnessProducerKind lhs, WitnessProducerKind rhs) {
                return static_cast<uint8_t>(lhs) < static_cast<uint8_t>(rhs);
              });
  }

  std::string ToString() const {
    if (!known)
      return llvm::formatv("unknown(reason={0})", unknownReason).str();

    std::string out = "{";
    for (size_t i = 0; i < kinds.size(); ++i) {
      if (i)
        out += ",";
      out += toString(kinds[i]);
    }
    out += "}";
    return out;
  }

  std::string PartitionValue(uint64_t witnessId) const {
    if (known)
      return ToString();
    return llvm::formatv("unknown:producer:witness#{0}:{1}", witnessId,
                         unknownReason)
        .str();
  }
};

struct WitnessEquivalenceKey {
  WitnessEquivalenceDimension targetPPTokens =
      WitnessEquivalenceDimension::Unknown("target-pp-tokens-not-carried");
  WitnessEquivalenceDimension suffixState =
      WitnessEquivalenceDimension::Unknown("suffix-state-not-carried");
  WitnessEquivalenceDimension preservedObservers =
      WitnessEquivalenceDimension::Unknown("preserved-observers-not-carried");
  WitnessEquivalenceDimension counterState =
      WitnessEquivalenceDimension::Unknown("counter-state-not-carried");
  WitnessProducerKindSet producerKinds =
      WitnessProducerKindSet::Unknown("producer-kind-not-carried");
  WitnessBoundaryClass boundaryClass = WitnessBoundaryClass::Unknown;
  WitnessDiagnosticClass diagnosticClass = WitnessDiagnosticClass::Unknown;
  WitnessCompositionClass compositionClass = WitnessCompositionClass::Unknown;

  bool HasUnknownDimensions() const {
    return !targetPPTokens.known || !suffixState.known ||
           !preservedObservers.known || !counterState.known ||
           !producerKinds.known ||
           boundaryClass == WitnessBoundaryClass::Unknown ||
           diagnosticClass == WitnessDiagnosticClass::Unknown ||
           compositionClass == WitnessCompositionClass::Unknown;
  }

  std::string UnknownDimensionSummary() const {
    std::string out;
    auto append = [&](llvm::StringRef name) {
      if (!out.empty())
        out += ",";
      out += name;
    };

    if (!targetPPTokens.known)
      append("target_pp");
    if (!suffixState.known)
      append("suffix_state");
    if (!preservedObservers.known)
      append("observers");
    if (!counterState.known)
      append("counter");
    if (!producerKinds.known)
      append("producer");
    if (boundaryClass == WitnessBoundaryClass::Unknown)
      append("boundary");
    if (diagnosticClass == WitnessDiagnosticClass::Unknown)
      append("diagnostics");
    if (compositionClass == WitnessCompositionClass::Unknown)
      append("composition");
    return out.empty() ? std::string("none") : out;
  }

  static std::string PartitionEnumValue(llvm::StringRef name,
                                        llvm::StringRef value, bool known,
                                        uint64_t witnessId) {
    if (known)
      return value.str();
    return llvm::formatv("unknown:{0}:witness#{1}", name, witnessId).str();
  }

  std::string PartitionString(uint64_t witnessId) const {
    return llvm::formatv(
               "target={0}|suffix={1}|observers={2}|counter={3}|"
               "producers={4}|boundary={5}|diagnostics={6}|"
               "composition={7}",
               targetPPTokens.PartitionValue("target_pp", witnessId),
               suffixState.PartitionValue("suffix_state", witnessId),
               preservedObservers.PartitionValue("observers", witnessId),
               counterState.PartitionValue("counter", witnessId),
               producerKinds.PartitionValue(witnessId),
               PartitionEnumValue(
                   "boundary", toString(boundaryClass),
                   boundaryClass != WitnessBoundaryClass::Unknown, witnessId),
               PartitionEnumValue("diagnostics", toString(diagnosticClass),
                                  diagnosticClass !=
                                      WitnessDiagnosticClass::Unknown,
                                  witnessId),
               PartitionEnumValue("composition", toString(compositionClass),
                                  compositionClass !=
                                      WitnessCompositionClass::Unknown,
                                  witnessId))
        .str();
  }

  std::string ToString() const {
    return llvm::formatv(
               "target_pp={0} suffix_state={1} observer_state={2} "
               "counter={3} producers={4} boundary={5} diagnostics={6} "
               "composition={7} complete={8} unknown_dims={9}",
               targetPPTokens.ToString(), suffixState.ToString(),
               preservedObservers.ToString(), counterState.ToString(),
               producerKinds.ToString(), toString(boundaryClass),
               toString(diagnosticClass), toString(compositionClass),
               HasUnknownDimensions() ? "no" : "yes", UnknownDimensionSummary())
        .str();
  }
};

struct WitnessCanonicalCost {
  uint64_t preserveOriginalPenalty = 0;
  uint64_t sourceRangeBytes = 0;
  uint64_t argumentBoundaryChangePenalty = 0;
  uint64_t ownerBoundaryChangePenalty = 0;
  uint64_t spellingChangePenalty = 0;
  uint64_t sourceOrder = 0;

  std::string ToString() const {
    return llvm::formatv("preserve_penalty={0} range_bytes={1} "
                         "arg_boundary_penalty={2} owner_boundary_penalty={3} "
                         "spelling_penalty={4} source_order={5}",
                         preserveOriginalPenalty, sourceRangeBytes,
                         argumentBoundaryChangePenalty,
                         ownerBoundaryChangePenalty, spellingChangePenalty,
                         sourceOrder)
        .str();
  }
};

/// Resolver-facing witness record for one accepted-result candidate.
///
/// The witness captures the candidate's proof family, producer, boundary,
/// diagnostic/composition classes, equivalence key, fallback class, and
/// canonical cost so resolver decisions can be audited without rereading the
/// original patch/edit artifact.
struct RefoldWitness {
  uint64_t witnessId = 0;
  WitnessProofFamily family = WitnessProofFamily::Unknown;
  std::string owner;
  std::string detail;
  // C1 closure-ledger metadata.  These fields are trace-only copies of the
  // selector/candidate provenance already used to build `detail`; keeping
  // them structured avoids parsing human-oriented trace text when grouping
  // missing proof dimensions by family and code path.
  std::string selector;
  std::string sourceFamily;
  std::string candidateKind;
  std::string theoremClass;
  /// Typed discriminator behind `sourceFamily` (which is only its `toString`
  /// spelling, kept for trace).  Resolver-authority decisions must compare this
  /// enum, not the string, so a `toString` rename cannot silently change which
  /// witnesses the resolver treats as authoritative.
  AcceptedPathKind sourcePathKind{};
  WitnessEquivalenceKey key;
  WitnessCanonicalCost cost;
  std::string payloadPreview;

  std::string ToString() const {
    return llvm::formatv("id={0} family={1} owner={2} detail={3}", witnessId,
                         toString(family), owner, detail)
        .str();
  }
};

struct WitnessAmbiguityClass {
  uint64_t classIndex = 0;
  WitnessEquivalenceKey key;
  uint64_t candidateCount = 0;
  uint64_t selectableCount = 0;

  std::string ToString() const {
    return llvm::formatv("class={0} candidates={1} selectable={2} {3}",
                         classIndex, candidateCount, selectableCount,
                         key.ToString())
        .str();
  }
};

/// \brief Probe result for composing locally selected witnesses into a
/// globally valid source-repair tuple.
///
/// This is a resolver-side proof object.  A selector candidate is either a
/// one-tile tuple or a pre-composed mixed-owner tiling witness.  Unknown
/// composition facts never establish equivalence; strict mode may use a
/// resolver result only when every selectable tuple has complete composition
/// facts and the surviving tuples occupy one global composition class, or
/// when multiple proof certificates certify the same concrete source repair.
struct WitnessCompositionDecision {
  bool computed = false;
  bool compatible = false;
  bool failureIsFatal = false;
  uint64_t candidateTupleCount = 0;
  uint64_t completeTupleCount = 0;
  uint64_t incompleteTupleCount = 0;
  uint64_t incompatibleTupleCount = 0;
  uint64_t globalClassCount = 0;
  std::string reason;
};

/// \brief One machine-readable C1 closure-ledger row.
///
/// A row is emitted only for decisions classified as
/// PotentiallyInDomainMissingProof.  It records the exact selector, proof
/// family, missing equivalence/proof dimension, and theorem obligation that
/// prevents resolver authority.  The row is purely diagnostic and must never
/// affect candidate ordering, admissibility, or emitted source text.
struct WitnessClosureLedgerEntry {
  std::string selector;
  std::string testRegion;
  uint64_t witnessId = 0;
  uint64_t candidateIndex = 0;
  WitnessProofFamily family = WitnessProofFamily::Unknown;
  std::string sourceFamily;
  std::string candidateKind;
  std::string theoremClass;
  std::string sourceOwnerKind;
  std::string owner;
  std::string missingDimension;
  std::string missingReason;
  WitnessStrictDomainObligation obligation =
      WitnessStrictDomainObligation::Unknown;
  WitnessFallbackClass fallbackClass = WitnessFallbackClass::Unknown;
  std::string resolverReason;
};

#define REFOLD_WITNESS_RESOLVER_MODE_LIST(REFOLD_X)                            \
  REFOLD_X(Off)                                                                \
  REFOLD_X(Probe)                                                              \
  REFOLD_X(Strict)

enum class WitnessResolverMode : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_RESOLVER_MODE_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessResolverMode value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case WitnessResolverMode::name:                                              \
    return #name;
    REFOLD_WITNESS_RESOLVER_MODE_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Off";
}
#undef REFOLD_WITNESS_RESOLVER_MODE_LIST

/// \brief Probe/strict decision from the central witness resolver.
///
/// In `off` mode the resolver is not run for ordinary non-strict refolding.
/// In `probe` mode it computes and logs agreement without authority.  In
/// `strict` mode it is authoritative only when every selectable witness comes
/// from a converted proof family, has a complete equivalence key, and the
/// surviving source-repair classes are equivalent under the resolver policy.
struct WitnessResolverDecision {
  std::string role;
  WitnessResolverMode mode = WitnessResolverMode::Off;
  uint64_t candidateCount = 0;
  uint64_t selectableCount = 0;
  uint64_t proofInvalidCount = 0;
  uint64_t equivalenceClassCount = 0;
  uint64_t completeWitnessCount = 0;
  uint64_t incompleteWitnessCount = 0;
  uint64_t resolverAuthoritativeWitnessCount = 0;
  uint64_t resolverUnconvertedWitnessCount = 0;
  WitnessCompositionDecision composition;
  std::optional<size_t> legacyIndex;
  std::optional<size_t> resolverIndex;
  bool resolverComputed = false;
  bool resolverImplemented = false;
  bool strictUseResolver = false;
  bool strictFailClosed = false;
  std::string failureReason;
  WitnessFallbackClass fallbackClass = WitnessFallbackClass::Unknown;
  WitnessStrictDomainDecision strictDomain;
  std::vector<WitnessClosureLedgerEntry> closureLedger;
  std::string agreement;

  bool ShouldUseResolverIndex() const {
    return mode == WitnessResolverMode::Strict && strictUseResolver &&
           resolverIndex.has_value();
  }

  bool ShouldFailClosed() const {
    return mode == WitnessResolverMode::Strict && strictFailClosed;
  }
};

/// Per-run theorem/proof accounting counters.
///
/// The engine and proof services update these counters while normalizing
/// accepted candidates, witness decisions, terminal fallback, and no-legacy
/// theorem-audit findings.
struct RefoldStats {
  uint64_t totalIncludes = 0;
  uint64_t expandedIncludes = 0;
  uint64_t totalMacros = 0;
  uint64_t expandedMacros = 0;
};

/// \brief Declared structural-refolding domain used by the theorem audit.
///
/// The theorem/completeness target is explicit so later work does not
/// silently move the goalposts. The engine treats an input as inside the
/// declared structural domain iff every emitted non-terminal artifact can be
/// represented by a normalized accepted-result carrier such that:
///   - the carrier belongs to a declared proof class,
///   - the carrier is explicit-proof-backed,
///   - the carrier is inside the declared domain,
///   - the carrier is locally discharged,
///   - final competition is resolved only by the shared lattice, and
///   - no unresolved owner / unresolved B-envelope exclusion remains except
///     those explicitly tracked as named out-of-domain terminal results.
///
/// Internal staging objects may temporarily lack one of these properties, but
/// they are not theorem-facing and therefore do not count toward the domain
/// statement until they are recertified onto a concrete emitted carrier.
struct TheoremAuditStats {
  uint64_t emittedNonTerminalEdits = 0;
  uint64_t emittedCarriers = 0;
  uint64_t emittedDeclaredClassCarriers = 0;
  uint64_t emittedDischargedCarriers = 0;
  uint64_t emittedSelectorOnlyExceptionCarriers = 0;
  uint64_t emittedTransitionalTheoremCarriers = 0;
  uint64_t emittedUndischargedCarriers = 0;
  uint64_t emittedUnknownClassCarriers = 0;
  uint64_t emittedOutOfDomainCarriers = 0;

  // composite edits must be proof-composed, not just bags of
  // individually valid carriers.  These counters distinguish the accepted
  // composition laws currently enforced at the emission boundary.
  uint64_t emittedCompositeEdits = 0;
  uint64_t emittedEquivalentCompositeEdits = 0;
  uint64_t emittedOrderedCompositeEdits = 0;
  uint64_t emittedUncomposedCompositeEdits = 0;

  uint64_t selectorCompetitions = 0;
  uint64_t selectorResolutions = 0;
  uint64_t selectorNoSelectable = 0;
  uint64_t selectorUnresolvedCompetitions = 0;
  uint64_t selectorDirectBypasses = 0;

  // 4: no-legacy audit findings are theorem-audit data, not
  // ad hoc stderr-only diagnostics.  The aggregate count covers every
  // emitted finding; the boundary/rejection counters keep the fail-closed
  // emission-boundary subset distinct from ordinary proof-discharge failures.
  uint64_t noLegacyAuditFindings = 0;
  uint64_t noLegacyEmissionBoundaryViolations = 0;
  uint64_t noLegacyStrictRejections = 0;

  uint64_t explicitTerminalExclusions = 0;
  uint64_t nonExplicitTerminalExclusions = 0;

  // terminal fallback must retain all failed obligations and
  // audit them before raw-B emission.
  uint64_t terminalFailureObligations = 0;
  uint64_t terminalSecondaryFailureObligations = 0;
  uint64_t terminalFailureAuditViolations = 0;

  // persistent owner/state graph census.  These counters are
  // diagnostic/audit data only; they make it visible whether zero-token state
  // events and missing producer facts were modeled in the graph.
  uint64_t graphOwnerNodes = 0;
  uint64_t graphZeroTokenStateNodes = 0;
  uint64_t graphObservedStateComponents = 0;
  uint64_t graphMutatedStateComponents = 0;
  uint64_t graphIncomparableNodes = 0;
  uint64_t graphMissingProducerFacts = 0;

  // state-changing edits must be audited through the
  // StateTransitionGateway before any non-terminal bytes are emitted.  These
  // counters are updated by the gateway itself, so the final emission audit
  // can reject Unknown/None/undischarged state transitions without trying to
  // rediscover state effects from emitted text.
  uint64_t stateTransitionGatewayChecks = 0;
  uint64_t stateTransitionGatewayStable = 0;
  uint64_t stateTransitionGatewayTerminalFailures = 0;
  uint64_t stateTransitionAuditViolations = 0;
  uint64_t stateTransitionUnknownComponentViolations = 0;
  uint64_t stateTransitionUnknownMutationViolations = 0;
  uint64_t stateTransitionNoneWitnessViolations = 0;

  // Direct state-sensitive handling inventory.  Strict/theorem validation
  // populates these counters to make remaining component-local state checks
  // visible until they are routed through the gateway.
  uint64_t directStateChecksAudited = 0;
  uint64_t directStateChecksDeltaFacts = 0;
  uint64_t directStateChecksGraphEdges = 0;
  uint64_t directStateChecksGatewayWitnesses = 0;
  uint64_t directStateChecksTerminalFailures = 0;
  uint64_t directStateChecksUnclosedLocal = 0;

  // Final strict-domain resolver audit. These counters make the theorem
  // boundary machine-checkable from one strict trace run: no missing-proof
  // domain, no strict legacy fallback, declared repairs have complete keys,
  // and fail-closed resolver outcomes are complete non-equivalent ambiguity.
  uint64_t resolverDomainAudits = 0;
  uint64_t resolverDeclaredInDomain = 0;
  uint64_t resolverExplicitOutOfDomain = 0;
  uint64_t resolverAmbiguousOutOfDomain = 0;
  uint64_t resolverPotentiallyMissingProof = 0;
  uint64_t resolverUnknownDomain = 0;
  uint64_t resolverStrictResolverAuthority = 0;
  uint64_t resolverStrictLegacyFallback = 0;
  uint64_t resolverStrictFailClosed = 0;
  uint64_t resolverStrictInvalidFailClosed = 0;
  uint64_t resolverDeclaredIncompleteKeys = 0;
  uint64_t resolverDeclaredIncompatibleComposition = 0;
  uint64_t resolverDeclaredUnconvertedWitnesses = 0;
  uint64_t resolverClosureLedgerRows = 0;

  bool theoremSatisfied = true;
  std::string firstViolation;
};

//===----------------------------------------------------------------------===//
// Terminal fallback proof vocabulary.
//
// The data-only carriers live here so accepted-result and owner-state value
// records can refer to terminal failures without dragging in the mutable
// request sink or theorem-audit service.  The sink and audit live in
// RefoldTerminalFallback.{h,cpp}.
//===----------------------------------------------------------------------===//

/// Named proof obligation that forced the terminal raw-B result.
///
/// Terminal fallback is part of the proof system, not a convenience escape.
/// Every request must name the theorem obligation that failed.  Keep these
/// obligations close to the closed-domain contract: owner closure,
/// deterministic tiling, state stability, producer-fact availability, and final
/// validation are the only acceptable reasons to emit raw B.
enum class TerminalFallbackObligationKind : uint8_t {
  Unknown,
  OwnerClosedCover,
  DeterministicMixedOwnerTiling,
  StateTransitionClosure,
  PragmaBoundaryKnown,
  LineControlStateProducerProven,
  CounterStateStabilizable,
  MacroStateStabilizable,
  IncludeGuardStateStabilizable,
  ConditionalStateStabilizable,
  ReverseSolvedDirectiveForbidden,
  InvocationPreservationWellFormed,
  ProducerFactsAvailable,
  FinalValidationSucceeded,
  IncludeRealizationBEnvelopeMapped,
  EmissionArtifactDischarged,
  EmissionEditSetComposable,
  TheoremAuditInvariantSatisfied,
};

llvm::StringRef toString(TerminalFallbackObligationKind obligation);

/// Concrete reason the terminal-fallback obligation failed.
///
/// This implementation-local reason preserves precise diagnostics before
/// normalization to the final theorem vocabulary below.  The reason names the
/// local domain wall; the obligation names the theorem condition.  Keeping both
/// makes terminal fallback auditable without having to reverse-engineer the
/// implementation site that requested it.
enum class TerminalFallbackFailureReason : uint8_t {
  Unknown,
  NoOwnerClosedCover,
  NoDeterministicMixedOwnerTiling,
  AmbiguousMixedOwnerTiling,
  StateTransitionConsumedAndObserved,
  UnknownPragmaCrossesBoundary,
  LineControlStateNotProducerProven,
  CounterStateNotStabilizable,
  MacroStateNotStabilizable,
  IncludeGuardStateNotStabilizable,
  ConditionalStateNotStabilizable,
  ReverseSolvedDirectiveRequired,
  MalformedInvocationPreservation,
  MissingProducerFacts,
  NoCanonicalSuffixOrder,
  ValidationFailure,
  NoTUAnchorForUnresolvedOwner,
  UnmappableIncludeBEnvelope,
  UndischargedEmissionArtifact,
  UncomposableEmissionEditSet,
  TheoremAuditInvariantViolation,
  UnclassifiedTerminalFallback,
};

llvm::StringRef toString(TerminalFallbackFailureReason reason);

/// Final theorem-facing terminal failure vocabulary.
///
/// Older/local failure reasons may remain for diagnostics, but every live
/// terminal fallback must normalize to one of these theorem failure kinds. This
/// is the canonical answer to: "which strict-domain obligation failed strongly
/// enough to justify the terminal raw-B carrier?"
enum class TheoremFallbackFailureKind : uint8_t {
  Unknown,
  NoOwnerClosedCover,
  NoDeterministicMixedOwnerTiling,
  AmbiguousMixedOwnerTiling,
  StateTransitionConsumedAndObserved,
  UnknownPragmaCrossesBoundary,
  LineControlStateNotProducerProven,
  CounterStateNotStabilizable,
  MacroStateNotStabilizable,
  IncludeGuardStateNotStabilizable,
  ConditionalStateNotStabilizable,
  ReverseSolvedDirectiveRequired,
  MalformedInvocationPreservation,
  MissingProducerFacts,
  ValidationFailure,
};

llvm::StringRef toString(TheoremFallbackFailureKind kind);

/// Normalize an implementation-local terminal reason to the frozen theorem
/// failure vocabulary.  This is the gate that lets legacy/local diagnostics
/// coexist with a small final theorem language.
std::optional<TheoremFallbackFailureKind>
NormalizeTerminalFallbackFailureReason(TerminalFallbackFailureReason reason);

/// Structured local facts attached to a terminal proof failure.
///
/// Terminal fallback context is data, not prose.  Most fallback sites cannot
/// populate every field yet, but the carrier is intentionally stable: later
/// proof passes can attach owner, hunk, state-component, source-span,
/// token-envelope, and first-observer evidence without changing the terminal
/// witness format again.  Logs are derived from this structure; callers should
/// not encode theorem facts only inside a free-form detail string.
struct TerminalFallbackFailureContext {
  std::optional<std::string> owner;

  /// Producer id of the region whose proof failed, when the site knows it.
  ///
  /// This is what lets a terminal request be narrowed instead of taken: the
  /// fallback ladder can rule out preserving that one region and re-assemble,
  /// rather than emitting the edited stream for the whole translation unit.
  /// Sites that genuinely have no region -- a translation-unit-wide producer
  /// inconsistency, say -- leave it absent and the terminal carrier stands.
  std::optional<uint64_t> ownerId;
  std::optional<uint64_t> hunk;
  std::optional<std::string> stateComponent;

  std::optional<std::string> sourcePath;
  std::optional<uint64_t> sourceBegin;
  std::optional<uint64_t> sourceEnd;

  std::optional<uint64_t> aTokenBegin;
  std::optional<uint64_t> aTokenEnd;
  std::optional<uint64_t> bTokenBegin;
  std::optional<uint64_t> bTokenEnd;

  static TerminalFallbackFailureContext ForStateComponent(llvm::StringRef name);
  static TerminalFallbackFailureContext ForOwnerId(uint64_t ownerId);
  static TerminalFallbackFailureContext
  ForHunkTokenEnvelope(uint64_t hunkIndex, uint64_t aBegin, uint64_t aEnd,
                       uint64_t bBegin, uint64_t bEnd);

  bool Empty() const;
  std::string ToString() const;
};

/// Normalized proof failure attached to terminal fallback.
struct TerminalFallbackProofFailure {
  TerminalFallbackObligationKind obligation =
      TerminalFallbackObligationKind::Unknown;
  TerminalFallbackFailureReason reason = TerminalFallbackFailureReason::Unknown;
  TheoremFallbackFailureKind theoremFailure =
      TheoremFallbackFailureKind::Unknown;
  TerminalFallbackFailureContext context;

  std::string ToString() const;
};

/// Return whether a fallback proof failure is theorem-facing.
///
/// Terminal fallback proofs may not still say `Unknown` or `Unclassified`.  The
/// guard is deliberately small and shared by request recording,
/// terminal-witness construction, and theorem-audit reporting so a new fallback
/// path cannot quietly reintroduce an opaque raw-B escape.
bool IsClassifiedTerminalFallbackProofFailure(
    const TerminalFallbackProofFailure &failure);

/// Build a classified terminal fallback proof failure.
///
/// Use this helper at every terminal fallback request site.  It documents the
/// local proof obligation being rejected and prevents call sites from relying
/// on terminal-fallback kind alone as an implicit classification.
TerminalFallbackProofFailure
MakeTerminalFallbackProofFailure(TerminalFallbackObligationKind obligation,
                                 TerminalFallbackFailureReason reason);

/// Build a classified terminal fallback proof failure with structured local
/// context.  This overload keeps context population centralized so call sites
/// do not have to duplicate the obligation/reason initialization sequence
/// before attaching owner, hunk, state, or span facts.
TerminalFallbackProofFailure
MakeTerminalFallbackProofFailure(TerminalFallbackObligationKind obligation,
                                 TerminalFallbackFailureReason reason,
                                 TerminalFallbackFailureContext context);

/// Compact witness describing why terminal raw-B emission was selected.
///
/// This is the theorem-facing carrier for the terminal raw-B exit.  It
/// deliberately keeps only the ordered structured failures.  There is no
/// parallel "primary" scalar, request counter, or branch-local boolean: the
/// first element is the primary failed obligation and the remaining elements
/// are secondary obligations discovered before terminal emission.
struct TerminalFallbackWitness {
  std::vector<TerminalFallbackProofFailure> proofFailures;

  const TerminalFallbackProofFailure *PrimaryFailure() const;
  std::string ToString() const;
};

/// Typed construction request for the terminal raw-B carrier.
///
/// Terminal fallback construction is data-first.  The proof failure below is
/// the only semantic reason that may justify selecting the terminal
/// out-of-domain result; `stage` and `detail` are diagnostic labels used to
/// keep traces actionable.  Keeping them in the same carrier prevents call
/// sites from smuggling behavior through a free-form reason string while
/// preserving the useful human explanation in debug output.
struct TerminalFallbackRequest {
  TerminalFallbackProofFailure failure;
  std::string stage;
  std::string detail;

  std::string ToString() const;
};

TerminalFallbackRequest
MakeTerminalFallbackRequest(TerminalFallbackProofFailure failure,
                            llvm::StringRef stage, llvm::StringRef detail);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFVOCABULARY_H
