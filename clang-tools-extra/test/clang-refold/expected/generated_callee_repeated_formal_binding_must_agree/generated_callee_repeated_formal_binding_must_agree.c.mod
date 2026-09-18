// RUN: %clang-refold-tester-verify-off generated_callee_repeated_formal_binding_must_agree
// Regression: every occurrence of a formal in a generated callee's replacement
// list must invert to the same actual.
//
// `CALL(DUP, a)` expands through the generated callee `DUP` to `a + a`, and the
// edit changes only the second operand to `b`.  The replay solver starts each
// formal at its old actual.  It used to treat a slot that still spelled the old
// actual as unbound, so the first `x` binding `a` left the slot open and the
// second `x` overwrote it with `b`.  That solved `x = b` uniquely and rewrote
// the callsite to `CALL(DUP, b)`, which expands to `b + b`.  With output
// verification off nothing caught it.
//
// No single actual reproduces `a + b`, so the invocation must not keep its
// callsite; it is realized by expansion instead.
#define CALL(f, v) f(v)
#define DUP(x) x + x

int a = 1, b = 2;
int r = a + b;
