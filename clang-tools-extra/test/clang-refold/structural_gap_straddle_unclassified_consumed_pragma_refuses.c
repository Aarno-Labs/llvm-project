// RUN: %clang-refold-tester-relaxed-expect-refold-fail structural_gap_straddle_unclassified_consumed_pragma_refuses
// Refusal shape: a payload straddling a pragma the preprocessor consumes but
// that no classification covers.
//
// This is the tripwire that
// `structural_gap_straddle_composed_define_and_consumed_pragma_commits_payload`
// used to be.  That file's pragma was `region`, which stopped being
// unclassified once the taxonomy read Clang's own handler for it, so it can no
// longer witness the fail-closed default.  `#pragma GCC dependency` can: Clang
// consumes it -- it emits nothing into the preprocessed stream, so the payload's
// side of it is not fixed by alignment -- and the taxonomy recognizes no such
// spelling.
//
// Two independent questions decide a pragma crossing, and this input answers
// only the first:
//
//   structure=Pragma ... crossable=false reason=PayloadObservesState
//
// The directive has no recorded image in A, so the emission question passes.
// The state question then fails: an unrecognized spelling is assumed to change
// arbitrary state and to bind whatever follows it, which is the only safe
// default for a pragma the engine has never seen.  Committing the payload to
// either side would be choosing a meaning rather than proving one.
//
// TRIAGE: correct, and the refusal is the assertion.  This shape must never
// fold, so it is pinned with the harness that requires the refold to fail
// rather than with `XFAIL`: an expected-failure entry says "not closed yet",
// which is the opposite of what this test means, and it would sit in the
// remaining-work count forever.  If the shape ever starts folding, this test
// fails -- which is the same alarm `XPASS` would have raised.
int arr[] = { 1,
#pragma GCC dependency "headers/gap_dependency.h"
2 };
