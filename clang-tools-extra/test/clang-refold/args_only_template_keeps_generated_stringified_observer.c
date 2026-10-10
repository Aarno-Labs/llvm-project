// RUN: %clang-refold-tester-verify-off args_only_template_keeps_generated_stringified_observer
// A template-solved args-only rewrite is refused when a token it holds fixed
// comes from a generated call whose argument provenance is unknown.
//
// `CALL PAIR` forms CALL's call from WRAP's replacement list and WRAP's
// actual, and CALL stringifies the element it receives.  WRAP's body spans
// cover that `"x"`, so the template solver held it fixed while solving PAIR
// from `int x`.  B renames only `int x`; rewriting PAIR to `(y)` would also
// turn the unchanged `"x"` into `"y"`.  Nothing records which of WRAP's
// formals reached CALL's argument, so the fold fails closed and the
// invocation is expanded.  This used to emit `WRAP((y))` at verify-off.
#define CALL(X) char *s = #X; int X;
#define WRAP(PAIR) CALL PAIR
WRAP((x))
