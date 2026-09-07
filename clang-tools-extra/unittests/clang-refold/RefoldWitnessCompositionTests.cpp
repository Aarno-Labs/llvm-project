//===-- RefoldWitnessCompositionTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for tuple-level composition compatibility.
//
// `ResolveWitnessComposition` partitions selectable tuples by equivalence key
// and refuses a multi-class composition unless every tuple emits one identical
// repair.  That exception is a theorem -- the state a tuple leaves for its
// neighbors follows from the bytes it emits over the range it replaces, not
// from the theorem that proved them admissible -- and it is needed because the
// key also carries proof *provenance*: each proof family authors its own
// spellings for the same facts, and `boundaryClass` / `producerKinds` describe
// the certificate rather than the resulting state.  So a class count above one
// means "not textually the same certificate", which is weaker than "these
// certificates disagree".
//
// Across the whole lit suite, every waiver-eligible composition differs in
// provenance only: of 169 such blocks, 72 differ in proof-family dialect and
// 97 in `boundaryClass`, and *none* differ in anything describing resulting
// state.  No lit test therefore separates the waiver from the fail-closed
// arms, and none distinguishes the arm *order* at all.  The facts the theorem
// rests on are pinned here instead:
//
//   * the premise's fail-closed direction -- an unknown repair is not
//     evidence that two certificates agree;
//   * the waiver itself, on the two provenance-only shapes that occur;
//   * that a distinct repair still fails closed;
//   * the two precedence facts, which a reordering of the arms would
//     silently reverse: a terminal tuple outranks the waiver, and an
//     incomplete key withholds compatibility without being fatal.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldWitnessResolver.h"

#include "gtest/gtest.h"

#include <string>
#include <utility>
#include <vector>

using namespace clang::refold;

namespace {

/// A witness whose every key dimension is known, so it reaches the class
/// partition rather than being counted as incomplete.
RefoldWitness completeWitness(uint64_t witnessId) {
  RefoldWitness witness;
  witness.witnessId = witnessId;
  witness.owner = "owner";
  witness.key.targetPPTokens = WitnessEquivalenceDimension::Known("target");
  witness.key.suffixState = WitnessEquivalenceDimension::Known("suffix");
  witness.key.preservedObservers =
      WitnessEquivalenceDimension::Known("observers");
  witness.key.counterState = WitnessEquivalenceDimension::Known("counter");
  witness.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::Forward);
  witness.key.boundaryClass = WitnessBoundaryClass::RootInvocation;
  witness.key.diagnosticClass = WitnessDiagnosticClass::PreservesDiagnostics;
  witness.key.compositionClass = WitnessCompositionClass::LocalOnly;
  return witness;
}

/// Attach the emitted-repair identity a candidate builder would record.
void setRepair(RefoldWitness &witness, uint64_t begin, uint64_t end,
               std::string text) {
  witness.emittedRepair =
      EmittedRepairIdentity{begin, end, std::move(text)};
}

std::vector<std::pair<size_t, RefoldWitness>>
tuples(std::vector<RefoldWitness> witnesses) {
  std::vector<std::pair<size_t, RefoldWitness>> out;
  for (size_t index = 0; index < witnesses.size(); ++index)
    out.emplace_back(index, witnesses[index]);
  return out;
}

TEST(RefoldWitnessComposition, OneClassComposesWithoutARepairIdentity) {
  // The baseline arm: agreement on the key is itself sufficient, so no
  // identity is required to get here.  Pinned so a later change cannot make
  // the waiver's premise a precondition of ordinary composition.
  const RefoldWitness first = completeWitness(1);
  const RefoldWitness second = completeWitness(2);

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/false);

  EXPECT_TRUE(decision.compatible);
  EXPECT_EQ(decision.reason, "single-global-composition-class");
  EXPECT_EQ(decision.globalClassCount, 1u);
  EXPECT_FALSE(decision.failureIsFatal);
}

TEST(RefoldWitnessComposition, IdenticalRepairComposesAcrossABoundaryClass) {
  // The 97-block shape: two certificates anchored differently -- a root
  // invocation and a zero-token boundary -- describing one repair.  The
  // anchor names which producer record the certificate used, not what state
  // the edit leaves, so the differing classes are provenance and the shared
  // repair decides.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  second.key.boundaryClass = WitnessBoundaryClass::ZeroTokenBoundary;
  setRepair(first, 10, 20, "repair");
  setRepair(second, 10, 20, "repair");

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/true);

  ASSERT_EQ(decision.globalClassCount, 2u);
  EXPECT_TRUE(decision.compatible);
  EXPECT_EQ(decision.reason, "identical-repair-composes-identically");
  EXPECT_FALSE(decision.failureIsFatal);
}

TEST(RefoldWitnessComposition, IdenticalRepairComposesAcrossProofFamilyDialect) {
  // The 72-block shape: one edit described by a stringification certificate
  // and by a token-paste certificate.  Each family authors its own spellings
  // for the same facts, so the key strings differ while the emitted bytes do
  // not.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  first.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::Stringify);
  second.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::PasteResult);
  second.key.suffixState =
      WitnessEquivalenceDimension::Known("suffix-paste-dialect");
  setRepair(first, 0, 4, "same");
  setRepair(second, 0, 4, "same");

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/true);

  ASSERT_EQ(decision.globalClassCount, 2u);
  EXPECT_TRUE(decision.compatible);
  EXPECT_EQ(decision.reason, "identical-repair-composes-identically");
}

TEST(RefoldWitnessComposition, DistinctRepairsFailClosed) {
  // Two classes and two repairs: the waiver's premise is absent, so this is a
  // genuine disagreement about what to emit and there is no ground for
  // choosing either.  This is the case the lit suite never produces.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  second.key.boundaryClass = WitnessBoundaryClass::ZeroTokenBoundary;
  setRepair(first, 10, 20, "one");
  setRepair(second, 10, 20, "another");

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/false);

  ASSERT_EQ(decision.globalClassCount, 2u);
  EXPECT_FALSE(decision.compatible);
  EXPECT_EQ(decision.reason, "multiple-non-equivalent-composition-classes");
  EXPECT_TRUE(decision.failureIsFatal);
}

TEST(RefoldWitnessComposition, AbsentRepairIdentityFailsClosed) {
  // The premise's conservative direction.  A tuple whose builder did not know
  // its emitted bytes carries no identity, which denies the whole set the
  // one-repair reading: an unknown repair is not evidence that two
  // certificates agree.  The caller reports that as a false flag, and the
  // waiver must not fire on it.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  second.key.boundaryClass = WitnessBoundaryClass::ZeroTokenBoundary;
  setRepair(first, 10, 20, "repair");
  ASSERT_FALSE(second.emittedRepair.has_value());

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/false);

  EXPECT_FALSE(decision.compatible);
  EXPECT_EQ(decision.reason, "multiple-non-equivalent-composition-classes");
  EXPECT_TRUE(decision.failureIsFatal);
}

TEST(RefoldWitnessComposition, TerminalTupleOutranksAnIdenticalRepair) {
  // Precedence, not classification.  A terminal tuple is out of the
  // composition domain outright, so it fails closed even when every tuple
  // emits the same bytes -- the waiver answers "which certificate?", never
  // "may this compose at all?".  Reordering the arms would reverse this
  // silently, because the terminal tuple is skipped before the class count is
  // taken and cannot show up as a second class.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  second.key.compositionClass = WitnessCompositionClass::Terminal;
  setRepair(first, 10, 20, "repair");
  setRepair(second, 10, 20, "repair");

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/true);

  EXPECT_FALSE(decision.compatible);
  EXPECT_EQ(decision.reason, "composition-incompatible-terminal-tuple");
  EXPECT_TRUE(decision.failureIsFatal);
  EXPECT_EQ(decision.incompatibleTupleCount, 1u);
}

TEST(RefoldWitnessComposition, IncompleteKeyWithholdsCompatibilityWithoutFailing) {
  // The other precedence fact, and the one asymmetry among the refusals: an
  // unknown dimension means the composition is unproven rather than
  // disproven, so it withholds compatibility without being fatal.  An
  // identical repair does not supply the missing dimension, so it must not
  // promote this to the waiver.
  RefoldWitness first = completeWitness(1);
  RefoldWitness second = completeWitness(2);
  second.key.counterState =
      WitnessEquivalenceDimension::Unknown("counter-state-not-carried");
  setRepair(first, 10, 20, "repair");
  setRepair(second, 10, 20, "repair");

  const WitnessCompositionDecision decision =
      RefoldWitnessResolver::ClassifyWitnessComposition(
          tuples({first, second}),
          /*hasSingleConcreteRepairIdentity=*/true);

  EXPECT_FALSE(decision.compatible);
  EXPECT_EQ(decision.reason, "composition-incomplete-witness-key");
  EXPECT_FALSE(decision.failureIsFatal);
  EXPECT_EQ(decision.incompleteTupleCount, 1u);
}

} // namespace
