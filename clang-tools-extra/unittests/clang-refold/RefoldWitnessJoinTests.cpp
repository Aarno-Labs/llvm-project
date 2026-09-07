//===-- RefoldWitnessJoinTests.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the certificate join that replaced the legacy-index bridge.
//
// When a selector carries several complete certificates for one edit, the
// resolver used to hand back whichever index its *caller* had already chosen
// and label that a resolver decision.  `ClassifyWitnessJoin` replaces that
// with a composed certificate: it checks that the certificates are about one
// edit and one B objective, agree on what the edit leaves observable, and
// then unions their producer obligations.  Selection follows from the
// canonical preference over the resolver's own selectable set.
//
// None of the refusal arms is reachable from lit.  Measured across the suite,
// the join fires 123 times in 23 files and is authorized every time: all four
// obligations hold in every competition the corpus produces, so no test file
// separates an authorized join from any of its refusals.  That is the same
// situation V2 and W1 documented, and the properties are pinned here instead:
//
//   * the authorized case, on the two provenance-only shapes that occur --
//     a stringification certificate beside a paste one, and an actual-repair
//     certificate beside a zero-token-boundary one;
//   * each obligation's refusal, separately, so that dropping any one of them
//     fails a named test rather than silently widening admission;
//   * that the target-envelope obligation is *independent* of the repair
//     identity, which is the obligation the bridge never had: identical bytes
//     over an identical source range do not establish that two certificates
//     realize the same part of the modified stream;
//   * that an uncarried envelope is not evidence of agreement, including
//     against another uncarried one.
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

/// A witness whose every key dimension is known, carrying one repair and one
/// objective, so it discharges every join obligation on its own.
RefoldWitness joinableWitness(uint64_t witnessId) {
  RefoldWitness witness;
  witness.witnessId = witnessId;
  witness.owner = "macro#411";
  witness.key.targetPPTokens =
      WitnessEquivalenceDimension::Known("candidate_b_tokens:[0,7):0xabc");
  witness.key.suffixState = WitnessEquivalenceDimension::Known("suffix");
  witness.key.preservedObservers =
      WitnessEquivalenceDimension::Known("observers");
  witness.key.counterState = WitnessEquivalenceDimension::Known("counter");
  witness.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::Stringify);
  witness.key.boundaryClass = WitnessBoundaryClass::RootInvocation;
  witness.key.diagnosticClass = WitnessDiagnosticClass::PreservesDiagnostics;
  witness.key.compositionClass = WitnessCompositionClass::LocalOnly;
  witness.key.targetEnvelope = WitnessTargetEnvelope::Known(0, 7, "0xabc");
  witness.emittedRepair = EmittedRepairIdentity{10, 20, "repair"};
  return witness;
}

std::vector<std::pair<size_t, RefoldWitness>>
tuples(std::vector<RefoldWitness> witnesses) {
  std::vector<std::pair<size_t, RefoldWitness>> out;
  for (size_t i = 0; i < witnesses.size(); ++i)
    out.push_back(std::make_pair(i, witnesses[i]));
  return out;
}

WitnessJoinDecision join(std::vector<RefoldWitness> witnesses) {
  return RefoldWitnessResolver::ClassifyWitnessJoin(
      tuples(std::move(witnesses)));
}

//===----------------------------------------------------------------------===//
// The authorized case.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, JoinsCertificatesDifferingOnlyInProvenance) {
  // The `#`-and-`##` shape the resolver's comment names, and 5 of the 123
  // joins the suite produces: one edit, described once by a stringification
  // certificate and once by a paste one.  Every dimension the two families
  // spell in their own vocabulary differs; every dimension the join checks
  // agrees.
  RefoldWitness stringify = joinableWitness(1);
  RefoldWitness paste = joinableWitness(2);
  paste.key.targetPPTokens =
      WitnessEquivalenceDimension::Known("token_paste_b_tokens:[0,7):0xabc");
  paste.key.suffixState =
      WitnessEquivalenceDimension::Known("suffix-paste-dialect");
  paste.key.preservedObservers =
      WitnessEquivalenceDimension::Known("observers-paste-dialect");
  paste.key.counterState =
      WitnessEquivalenceDimension::Known("counter-paste-dialect");
  paste.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::PasteResult);

  const WitnessJoinDecision decision = join({stringify, paste});

  EXPECT_TRUE(decision.computed);
  EXPECT_TRUE(decision.authorized);
  EXPECT_EQ(decision.reason, "joined-certificates-authorize-one-repair");
  EXPECT_EQ(decision.certificateCount, 2u);
  ASSERT_TRUE(decision.repair.has_value());
  EXPECT_EQ(decision.repair->text, "repair");
  EXPECT_TRUE(decision.objective.known);
  EXPECT_EQ(decision.objective.bTokBegin, 0u);
  EXPECT_EQ(decision.objective.bTokEnd, 7u);
  EXPECT_EQ(decision.diagnosticClass,
            WitnessDiagnosticClass::PreservesDiagnostics);
  EXPECT_EQ(decision.compositionClass, WitnessCompositionClass::LocalOnly);
}

TEST(RefoldWitnessJoin, JoinsAcrossDisagreeingBoundaryClass) {
  // The dominant shape: 95 of the 123 joins pair an actual-repair certificate
  // with a zero-token-boundary one, and `boundaryClass` describes which anchor
  // a certificate used rather than what the edit leaves behind.  It is
  // therefore deliberately not a join obligation.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.boundaryClass = WitnessBoundaryClass::ZeroTokenBoundary;

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_TRUE(decision.authorized);
}

TEST(RefoldWitnessJoin, ComposedProducerObligationIsTheUnion) {
  // The composed certificate discharges every producer obligation its
  // constituents do, and the set is order-normalized so the composition is a
  // function of the inputs rather than of their push order.
  RefoldWitness stringify = joinableWitness(1);
  RefoldWitness paste = joinableWitness(2);
  paste.key.producerKinds =
      WitnessProducerKindSet::KnownSingle(WitnessProducerKind::PasteResult);

  const WitnessJoinDecision forward = join({stringify, paste});
  const WitnessJoinDecision reverse = join({paste, stringify});

  ASSERT_TRUE(forward.authorized);
  ASSERT_TRUE(reverse.authorized);
  EXPECT_EQ(forward.producerKinds.kinds.size(), 2u);
  EXPECT_EQ(forward.producerKinds.ToString(), reverse.producerKinds.ToString());
}

TEST(RefoldWitnessJoin, SingleCertificateJoinsToItself) {
  const WitnessJoinDecision decision = join({joinableWitness(1)});

  EXPECT_TRUE(decision.authorized);
  EXPECT_EQ(decision.certificateCount, 1u);
}

//===----------------------------------------------------------------------===//
// One edit.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, DistinctRepairsFailClosed) {
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.emittedRepair = EmittedRepairIdentity{10, 20, "different"};

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-distinct-emitted-repairs");
  EXPECT_EQ(decision.failureClass,
            WitnessFallbackClass::MultipleNonEquivalentWitnessClasses);
}

TEST(RefoldWitnessJoin, UnknownRepairFailsClosed) {
  // A certificate whose builder did not record the bytes it emits denies the
  // join its premise.  It must not be read as agreeing by default.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.emittedRepair.reset();

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-unknown-emitted-repair");
  EXPECT_EQ(decision.failureClass, WitnessFallbackClass::IncompleteWitnessKey);
}

//===----------------------------------------------------------------------===//
// One objective.  This is the obligation the legacy-index bridge never had.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, SameRepairDifferentObjectiveFailsClosed) {
  // The two certificates emit the same bytes over the same source range and
  // still realize different parts of B.  The repair identity alone -- the
  // bridge's whole premise -- cannot tell these apart, which is why the
  // objective is checked separately rather than inferred from it.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.targetEnvelope = WitnessTargetEnvelope::Known(9, 16, "0xabc");
  ASSERT_EQ(*first.emittedRepair, *second.emittedRepair);

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-distinct-target-envelopes");
  EXPECT_EQ(decision.failureClass,
            WitnessFallbackClass::MultipleNonEquivalentWitnessClasses);
}

TEST(RefoldWitnessJoin, SameEnvelopeRangeDifferentContentFailsClosed) {
  // Same B-token range, different B content over it: still two objectives.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.targetEnvelope = WitnessTargetEnvelope::Known(0, 7, "0xdef");

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-distinct-target-envelopes");
}

TEST(RefoldWitnessJoin, UnknownObjectiveFailsClosed) {
  // The `empty_b_gap` and `terminal_full_b` target shapes carry no B-token
  // envelope.  A certificate on one of those cannot be joined, rather than
  // being joined on the strength of the dimensions it does carry.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.targetEnvelope = WitnessTargetEnvelope();

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-unknown-target-envelope");
  EXPECT_EQ(decision.failureClass,
            WitnessFallbackClass::UnknownTargetPreprocessedTokens);
}

TEST(RefoldWitnessJoin, TwoUncarriedObjectivesDoNotAgree) {
  // "Not carried" is not a value two certificates can share.  If unknown
  // envelopes compared equal, every certificate pair lacking the dimension
  // would join on the absence of evidence.
  const WitnessTargetEnvelope absent;
  EXPECT_FALSE(absent == WitnessTargetEnvelope());
  EXPECT_TRUE(absent != WitnessTargetEnvelope());

  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  first.key.targetEnvelope = WitnessTargetEnvelope();
  second.key.targetEnvelope = WitnessTargetEnvelope();

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-unknown-target-envelope");
}

//===----------------------------------------------------------------------===//
// Agreement on what the edit leaves observable.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, DistinctDiagnosticClassFailsClosed) {
  // Unlike the suffix and observer dimensions, this is a family-independent
  // enumeration: one certificate claiming the edit preserves diagnostics
  // while another claims it realizes an edited surface is a disagreement, not
  // a dialect.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.diagnosticClass = WitnessDiagnosticClass::RealizesEditedSurface;

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-distinct-diagnostic-classes");
  EXPECT_EQ(decision.failureClass,
            WitnessFallbackClass::MultipleNonEquivalentWitnessClasses);
}

TEST(RefoldWitnessJoin, DistinctCompositionClassFailsClosed) {
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.compositionClass = WitnessCompositionClass::OwnerClosed;

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-distinct-composition-classes");
}

//===----------------------------------------------------------------------===//
// Completeness, and the empty set.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, IncompleteKeyFailsClosed) {
  // A dimension that is not carried is not a dimension that agrees, so an
  // incomplete certificate cannot contribute to a join even when its repair
  // and objective match.
  RefoldWitness first = joinableWitness(1);
  RefoldWitness second = joinableWitness(2);
  second.key.suffixState =
      WitnessEquivalenceDimension::Unknown("suffix-not-carried");
  ASSERT_TRUE(second.key.HasUnknownDimensions());

  const WitnessJoinDecision decision = join({first, second});

  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "join-incomplete-witness-key");
  EXPECT_EQ(decision.failureClass, WitnessFallbackClass::IncompleteWitnessKey);
}

TEST(RefoldWitnessJoin, EmptyCertificateSetFailsClosed) {
  const WitnessJoinDecision decision = join({});

  EXPECT_TRUE(decision.computed);
  EXPECT_FALSE(decision.authorized);
  EXPECT_EQ(decision.reason, "no-certificate-to-join");
  EXPECT_EQ(decision.failureClass, WitnessFallbackClass::NoSelectableWitness);
}

//===----------------------------------------------------------------------===//
// The envelope is carried for the join alone.
//===----------------------------------------------------------------------===//

TEST(RefoldWitnessJoin, EnvelopeIsOutsideThePartitionAndTheCompletenessTest) {
  // Adding the unlabelled objective to either would repartition every
  // candidate in the tool -- the provenance-free-key change that is still
  // owed its own justification.  The join exists so that change is not
  // needed to ask whether two certificates are about one objective.
  RefoldWitness witness = joinableWitness(1);
  const std::string withEnvelope = witness.key.PartitionString(1);
  const bool completeWithEnvelope = !witness.key.HasUnknownDimensions();

  witness.key.targetEnvelope = WitnessTargetEnvelope();

  EXPECT_EQ(witness.key.PartitionString(1), withEnvelope);
  EXPECT_EQ(!witness.key.HasUnknownDimensions(), completeWithEnvelope);
  EXPECT_TRUE(completeWithEnvelope);
}

} // namespace
