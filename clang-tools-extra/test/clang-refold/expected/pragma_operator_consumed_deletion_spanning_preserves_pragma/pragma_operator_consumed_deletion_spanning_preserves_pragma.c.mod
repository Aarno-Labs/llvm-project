// RUN: %clang-refold-tester pragma_operator_consumed_deletion_spanning_preserves_pragma
// Regression: a pure deletion spanning a pragma written with the `_Pragma`
// operator tiles around it, exactly as the same deletion spanning the
// `#pragma` spelling already did.
//
// The two spellings are interchangeable to the preprocessor, and neither
// contributes a token to B, so nothing about this deletion depends on which one
// was written.  What separated them was not a proof at all: the producer records
// an operator-spelled pragma twice -- once as the pragma it performs and once as
// the `_Pragma` macro invocation that performed it -- at nested source ranges,
// and the structural gap theorem rejects every overlap between two proof-only
// owners that nothing explains.  The pragma owner now declares that its own
// proof covers a macro invocation nested inside its operator range, which is the
// producer's own account of one construct recorded twice.  An ordinary `#pragma`
// line still rejects a nested invocation.
//
// Both spellings of `region` remain unclassified, so this pins the deletion
// only: the straddling *replacement* across the same pragma still refuses, in
// `undetermined_payload_side_refuses_without_dropping_structure`.
int arr[] = { 
_Pragma("region")
 };
