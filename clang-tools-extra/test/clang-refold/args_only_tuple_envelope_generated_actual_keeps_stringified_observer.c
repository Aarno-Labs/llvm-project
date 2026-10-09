// RUN: %clang-refold-tester-verify-off args_only_tuple_envelope_generated_actual_keeps_stringified_observer
// An args-only rewrite is refused when an unchanged token comes from a
// generated call whose arguments came through a caller formal that its
// recorded text does not name.
//
// `F PAIR` forms ADD's call from two of CALL's actuals.  The text the producer
// records for it, `ADD, (x, z)`, delimits `x` and `z`, but they came through
// PAIR.  The producer used to scan that text for CALL's formal names and
// record `arg_deps: [[], []]`, which told the descendant gate that ADD's `"x"`
// ignores PAIR.  B renames `int x` and the root's own `#PAIR`; rewriting PAIR
// to `(y, z)` would also turn the unchanged `"x"` into `"y"`.  The fold fails
// closed and the invocation is expanded.  This used to emit
// `CALL(ADD, (y, z))` at verify-off.
#define ADD(a, b) int a; char *s = #a;
#define CALL(F, PAIR) F PAIR char *t = #PAIR;
CALL(ADD, (x, z))
