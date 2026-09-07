//===--- RefoldCompletenessTypes.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Completeness and theorem-domain contracts for clang-refold.
//
// The engine's completeness claim is explicit rather than implied: these
// records state whether a given acceptance path participates in the declared
// domain, what promise is made for it, and where a summary sits relative to
// that domain.  They make the scope of the claim auditable instead of leaving
// it to be inferred from which paths happen to be implemented.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOMPLETENESSTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOMPLETENESSTYPES_H

#include "proof/RefoldAcceptancePathTypes.h"

#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace clang {
namespace refold {

using llvm::StringRef;

/// \brief Whether an accepted path currently participates in the declared
/// completeness set.
///
/// This does not claim the engine is globally complete yet. Instead it makes
/// the scope of the completeness claim explicit: accepted paths either
/// already correspond to a declared proof class, remain transitional while a
/// class is still being closed, or sit outside the declared class set
/// entirely (for example an explicit terminal out-of-domain result).
#define REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)                       \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DeclaredProofClass)                                                 \
  REFOLD_X(TransitionalGap)                                                    \
  REFOLD_X(ExplicitOutOfDomainClass)

enum class CompletenessCoverageKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(CompletenessCoverageKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case CompletenessCoverageKind::name:                                         \
    return #name;
    REFOLD_COMPLETENESS_COVERAGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_COMPLETENESS_COVERAGE_KIND_LIST

/// \brief What completeness promise the engine makes for a covered path.
#define REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)                    \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MustDiscoverDeclaredOrStrongerCompatible)                           \
  REFOLD_X(NoClaimPendingClassClosure)                                         \
  REFOLD_X(ExplicitlyOutsideDeclaredSet)

enum class CompletenessExpectationKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(CompletenessExpectationKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case CompletenessExpectationKind::name:                                      \
    return #name;
    REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_COMPLETENESS_EXPECTATION_KIND_LIST

/// \brief Normalized completeness contract for the declared domain above.
///
/// This contract answers only one question: whether a theorem-facing carrier
/// already lies in the declared proof-class set, remains an internal-only
/// transitional gap that must not reach emission, or is explicitly outside
/// the declared set as a named terminal boundary.
struct CompletenessContract {
  CompletenessCoverageKind coverage = CompletenessCoverageKind::Unknown;
  CompletenessExpectationKind expectation =
      CompletenessExpectationKind::Unknown;
  FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
  bool countsTowardDeclaredCoverage = false;
  bool hasExplicitExclusion = false;
  TheoremFallbackFailureKind explicitExclusion =
      TheoremFallbackFailureKind::Unknown;
};

/// \brief The summary's position relative to the declared theorem domain.
///
/// This enum is the theorem-domain projection of the same declared-domain
/// statement: theorem-facing results are either in-domain declared proof
/// classes or explicit named out-of-domain classes. Transitional states may
/// still exist internally, but they are not allowed to survive to emission.
#define REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)                              \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(DeclaredInDomainClass)                                              \
  REFOLD_X(TransitionalGap)                                                    \
  REFOLD_X(ExplicitOutOfDomainClass)

enum class TheoremDomainKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(TheoremDomainKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case TheoremDomainKind::name:                                                \
    return #name;
    REFOLD_THEOREM_DOMAIN_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_THEOREM_DOMAIN_KIND_LIST

/// \brief Explicit theorem-domain contract derived from the same statement.
///
/// The theorem-domain view must say the same thing as the completeness view:
/// theorem-facing carriers are either in-domain declared proof classes or
/// explicit out-of-domain classes. Transitional states may still exist
/// internally, but they must remain non-emitting staging objects.
struct TheoremDomainContract {
  TheoremDomainKind kind = TheoremDomainKind::Unknown;
  bool inDeclaredDomain = false;
  bool countsTowardCompleteness = false;
  FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
  bool hasExplicitExclusion = false;
  TheoremFallbackFailureKind explicitExclusion =
      TheoremFallbackFailureKind::Unknown;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCOMPLETENESSTYPES_H
