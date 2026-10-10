// XFAIL: *
// RUN: %clang-refold-tester generated_call_output_insertion_after_consumed_source_refolds
// Not yet implemented: an insertion after a generated call's output should be
// made in source after the call.  `CALL(G, x)` leaves `G(x` open and the second
// `)` closes it.  B appends ` = 1` to `g_x`, and `CALL(G, x)) = 1;`
// preprocesses to B with both macros kept.  Today the callsite is expanded.
//
// `M(x) = 1;` folds for `#define M(a) int k; int a`, and so does a nested call
// spelled in M's own body: the insertion is made in source at M's end.  Here
// the token before the insertion is the output of G, whose name is the `G`
// that CALL received.  The insertion goes to the macro owners instead, and
// only the whole cover admits it.  The same happens to `CALL(G, x);` when
// CALL's body closes `G(a)` itself.  Folding needs the insertion routed to
// source after the end of the source the expansion consumed, the second `)`.
// After CALL's own `)` the inserted tokens would join G's argument.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, x)) = 1;
