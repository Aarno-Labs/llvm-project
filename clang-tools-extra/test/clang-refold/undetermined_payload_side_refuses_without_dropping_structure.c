// RUN: %clang-refold-tester-relaxed-expect-refold-fail undetermined_payload_side_refuses_without_dropping_structure
// Fail-closed regression: a refusal must not be paid for with the file.
//
// The payload is one token replacing material on both sides of the pragma, so
// its side is not determined by alignment.  Unlike the consumed pragmas whose
// insensitivity is provable, this spelling is unrecognized, and the taxonomy
// treats anything unclassified as observing everything.  Refusing is therefore
// correct: a pragma may change compiled meaning without changing the
// preprocessed tokens, so both placements replay B while differing in what they
// mean, and the closing check cannot separate them.
//
// PINS A REFUSAL, NOT AN OUTPUT.  What this asserts is that the refusal costs
// nothing beyond itself.  Emitting the edited preprocessed stream would also
// replay B -- it is B -- while deleting this comment and the pragma with it,
// which is the one result the refolder may never produce.  So the absence of a
// proof is reported as the absence of an answer.
//
// If a later theorem determines this payload's side, this test should stop
// refusing and pin the refold instead.
int arr[] = { 1,
#pragma region
2 };
