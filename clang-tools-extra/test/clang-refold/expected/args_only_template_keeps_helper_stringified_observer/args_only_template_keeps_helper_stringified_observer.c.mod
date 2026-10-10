// RUN: %clang-refold-tester-verify-off args_only_template_keeps_helper_stringified_observer
// A template-solved args-only rewrite is refused when a token it holds fixed
// is derived from the rewritten formal.
//
// The template solver solves C's formal slots and matches the rest of the
// expansion literally against B, reading the producer's body spans as fixed
// replacement-list text.  Those spans also cover S's `"x"`, which stringifies
// `a`.  B renames only `int x`; rewriting the actual to `y` would also turn
// the unchanged `"x"` into `"y"`.  S's arg_deps prove that dependence, so the
// fold fails closed and the invocation is expanded.  This used to emit `C(y)`
// at verify-off.
#define S(v) #v
#define C(a) char *s = S(a); int a;
char *s = "x"; int y;
