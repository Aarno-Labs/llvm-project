//===--- RefoldWitnessTypes.h ----------------------------------*- C++ -*-===//
//
// Theorem-facing witness and audit carrier records for clang-refold.
//
// These value-only records intentionally live outside RefoldEngine so extracted
// proof services can exchange witness vocabulary without friend access.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTYPES_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

/// \brief Vocabulary for behaviorally legacy emission paths.
///
/// A path is legacy only when emitted behavior is justified by one of these
/// implementation-local mechanisms instead of by the proof lattice.  This is
/// intentionally a definition layer, not an audit: no caller is rejected
/// merely by naming a kind here.  Audit sites use this shared vocabulary so
/// every diagnostic answers the same question: which old behavior must be
/// represented by which proof-lattice invariant before it is allowed to
/// survive to selection or emission?
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

inline LegacyPathDefinition DescribeLegacyPathKind(LegacyPathKind kind) {
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

inline bool IsDefinedLegacyPathKind(LegacyPathKind kind) {
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
#define REFOLD_X(name) case WitnessProofFamily::name: return #name;
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
#define REFOLD_X(name) case WitnessProducerKind::name: return #name;
    REFOLD_WITNESS_PRODUCER_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_PRODUCER_KIND_LIST

#define REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)                          \
REFOLD_X(Unknown)                                                           \
REFOLD_X(RootInvocation)                                                    \
REFOLD_X(NestedInvocation)                                                  \
REFOLD_X(MacroArgument)                                                     \
REFOLD_X(IncludeBoundary)                                                   \
REFOLD_X(TUAnchorBoundary)                                                  \
REFOLD_X(ZeroTokenBoundary)                                                 \
REFOLD_X(OwnerRealizationBoundary)                                          \
REFOLD_X(TerminalBoundary)

enum class WitnessBoundaryClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessBoundaryClass value) {
  switch (value) {
#define REFOLD_X(name) case WitnessBoundaryClass::name: return #name;
    REFOLD_WITNESS_BOUNDARY_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_BOUNDARY_CLASS_LIST

#define REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)                        \
REFOLD_X(Unknown)                                                           \
REFOLD_X(PreservesDiagnostics)                                              \
REFOLD_X(RealizesEditedSurface)                                             \
REFOLD_X(TerminalOutOfDomain)

enum class WitnessDiagnosticClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessDiagnosticClass value) {
  switch (value) {
#define REFOLD_X(name) case WitnessDiagnosticClass::name: return #name;
    REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_DIAGNOSTIC_CLASS_LIST

#define REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)                       \
REFOLD_X(Unknown)                                                           \
REFOLD_X(LocalOnly)                                                         \
REFOLD_X(OwnerClosed)                                                       \
REFOLD_X(MixedOwnerTile)                                                    \
REFOLD_X(Terminal)

enum class WitnessCompositionClass : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessCompositionClass value) {
  switch (value) {
#define REFOLD_X(name) case WitnessCompositionClass::name: return #name;
    REFOLD_WITNESS_COMPOSITION_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_COMPOSITION_CLASS_LIST

#define REFOLD_WITNESS_REJECT_REASON_LIST(REFOLD_X)                           \
REFOLD_X(Unknown)                                                           \
REFOLD_X(NotSelectable)                                                     \
REFOLD_X(ProofNormalizationFailed)                                          \
REFOLD_X(MissingEquivalenceKey)                                             \
REFOLD_X(ProducerKindMismatch)                                              \
REFOLD_X(StateUnknown)                                                      \
REFOLD_X(NonEquivalentAmbiguity)                                            \
REFOLD_X(SelectorOnlyNoEmittedCandidate)                                    \
REFOLD_X(TerminalFallbackRequested)

enum class WitnessRejectReason : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_REJECT_REASON_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessRejectReason value) {
  switch (value) {
#define REFOLD_X(name) case WitnessRejectReason::name: return #name;
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
#define REFOLD_X(name) case WitnessStrictDomainClass::name: return #name;
    REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_WITNESS_STRICT_DOMAIN_CLASS_LIST

#define REFOLD_WITNESS_STRICT_DOMAIN_OBLIGATION_LIST(REFOLD_X)                \
REFOLD_X(Unknown)                                                           \
REFOLD_X(FiniteDeterministicTiling)                                         \
REFOLD_X(ProducerProvenSourceWitness)                                       \
REFOLD_X(OwnerClosure)                                                      \
REFOLD_X(TargetPreprocessedTokens)                                          \
REFOLD_X(StateEquivalence)                                                  \
REFOLD_X(LineControlObserverEquivalence)                                    \
REFOLD_X(CounterEquivalence)                                                \
REFOLD_X(ModeledProducerSemantics)                                          \
REFOLD_X(ValidSourceRepair)                                                 \
REFOLD_X(Composition)                                                       \
REFOLD_X(ConvertedProofFamily)                                              \
REFOLD_X(CompleteWitnessKey)                                                \
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
#define REFOLD_X(name) case WitnessStrictDomainObligation::name: return #name;
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
  WitnessStrictDomainClass domainClass =
      WitnessStrictDomainClass::Unknown;
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
/// until a later proof stage supplies a real semantic value.
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

  std::string PartitionValue(llvm::StringRef name,
                             uint64_t witnessId) const {
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
      out += toString(kinds[i]).str();
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
  WitnessDiagnosticClass diagnosticClass =
      WitnessDiagnosticClass::Unknown;
  WitnessCompositionClass compositionClass =
      WitnessCompositionClass::Unknown;

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
      out += name.str();
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
                                        llvm::StringRef value,
                                        bool known, uint64_t witnessId) {
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
                   boundaryClass != WitnessBoundaryClass::Unknown,
                   witnessId),
               PartitionEnumValue(
                   "diagnostics", toString(diagnosticClass),
                   diagnosticClass != WitnessDiagnosticClass::Unknown,
                   witnessId),
               PartitionEnumValue(
                   "composition", toString(compositionClass),
                   compositionClass != WitnessCompositionClass::Unknown,
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
               HasUnknownDimensions() ? "no" : "yes",
               UnknownDimensionSummary())
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
    return llvm::formatv(
               "preserve_penalty={0} range_bytes={1} "
               "arg_boundary_penalty={2} owner_boundary_penalty={3} "
               "spelling_penalty={4} source_order={5}",
               preserveOriginalPenalty, sourceRangeBytes,
               argumentBoundaryChangePenalty, ownerBoundaryChangePenalty,
               spellingChangePenalty, sourceOrder)
        .str();
  }
};

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
  WitnessEquivalenceKey key;
  WitnessCanonicalCost cost;
  std::string payloadPreview;

  std::string ToString() const {
    return llvm::formatv("id={0} family={1} owner={2} detail={3}",
                         witnessId, toString(family), owner, detail)
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

#define REFOLD_WITNESS_RESOLVER_MODE_LIST(REFOLD_X)                           \
REFOLD_X(Off)                                                               \
REFOLD_X(Probe)                                                             \
REFOLD_X(Strict)

enum class WitnessResolverMode : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_WITNESS_RESOLVER_MODE_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(WitnessResolverMode value) {
  switch (value) {
#define REFOLD_X(name) case WitnessResolverMode::name: return #name;
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
/// statement until they are restamped onto a concrete emitted carrier.
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

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSTYPES_H
