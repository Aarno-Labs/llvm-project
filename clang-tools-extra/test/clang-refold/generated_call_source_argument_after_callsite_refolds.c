// XFAIL: *
// RUN: %clang-refold-tester generated_call_source_argument_after_callsite_refolds
// Not yet implemented: an edit to an argument that a generated call reads from
// source after the callsite should be made in place.  `CALL(ADD, 1)` leaves
// `ADD(1` open and `, 2)` completes it, so the `2` is ADD's `b`, spelled in
// source and copied once into the output.  B edits it, and `CALL(ADD, 1), 7);`
// preprocesses to B with both macros kept.  Today the callsite is expanded.
//
// ADD's args-only rewrite cannot reach the `2`.  ADD's recorded invocation
// text, `ADD, 1), 2)`, runs from the `ADD` that CALL received to the closing
// `)`, so it is not ADD's spelling, and the producer records no argument
// ranges or spans for it.  The tokmap does map the `2` to its source byte, but
// that alone does not prove a byte edit sound: it cannot show that `b` reaches
// the output only as a plain copy, and not also through a paste or a
// stringification the edit would leave stale.  Folding needs the producer to
// record a generated call's source-spelled actuals, and a proof of that.
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
CALL(ADD, 1), 2);
