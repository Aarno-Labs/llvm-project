// RUN: %clang-refold-tester-verify-off macro_state_sideband_argument_fold_keeps_consumed_define
// Regression: the first edit consumes `#define DIAG(x) _Pragma(#x)`, and the
// edited pragma is folded back into the argument of the `DIAG` invocation,
// which re-spells that invocation.  The callsite counted as consumed by the
// sideband edit, so nothing kept the definition and `DIAG` reached the output
// unexpanded.  A callsite an edit re-spells now survives, and the definition
// is kept for it.
int a1[] = { 77
#define DIAG(x) _Pragma(#x)
 };
DIAG(message("bye2"))
int v = 1;
