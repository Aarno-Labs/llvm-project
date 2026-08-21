// RUN: %clang-refold-tester-expect-refold-fail-verify-off macro_state_payload_names_live_macro_no_repair_refuses
// Soundness regression: a B payload naming a macro live where the payload lands,
// with neither macro-state repair available, must refuse rather than emit.
//
// B is a preprocessed stream, so the `ZZ` it carries is an identifier and the
// refold has to keep it one.  `ZZ` is bound at the replay position and expands
// to `VALUE` and thence to `3`, so emitting the payload under that binding
// re-expands a token B already finished expanding.
//
// Both repairs are blocked here, deliberately and independently.  `int before`
// observes `ZZ` ahead of the edit, so the definition cannot be carried past the
// replacement.  `int later` observes it after, so it cannot be undefined before
// the replacement without a restore.  Each repair declines on its own
// obligations, and each used to decline by simply moving on -- so the payload
// was emitted under a macro environment B never had, `--check` rejected the
// tool's own output, and the exit status was 0.
//
// Nothing in the suite could see it: `refold_tester.py` always passes
// `--verify-output=fatal`, which does catch the divergence, while the tool's
// own default is `off`.  This test therefore asserts on the refold's exit
// status, not on a `.c.mod`: `--expect-refold-fail` requires clang-refold to
// exit non-zero, so if the payload is ever emitted here again the test fails
// loudly instead of passing on a mismatched diff.
//
// TRIAGE: incompleteness once it refuses.  A repair bracketing the undef and a
// restore around the payload alone would realize this, which is the TU-side
// twin of the header repair in
// `materialized_header_macro_state_payload_and_preserved_source_share_line`.
// When that lands, this test folds and must be re-pointed at the fold.
#define VALUE 3
#define ZZ VALUE
int before = ZZ;
int arr[] = { 1, 2 };
int later = ZZ;
