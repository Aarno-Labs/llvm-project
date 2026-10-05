// RUN: %clang-refold-tester-verify-off args_only_paste_formal_also_selects_pasted_callee
// An args-only rewrite must not change a callee that the rewritten formal
// selects through a paste in a generated invocation.
//
// `n` is pasted into `n##_tag` and, through `CAT(impl_, n)`, into the callee
// name `impl_foo`.  B renames only the tag, so rewriting the actual to `bar`
// would also call `impl_bar` and turn the unchanged `foo_body` into
// `bar_body`.  The producer records the callee's root argument slice, so the
// rewrite is refused and the invocation is expanded.  This used to emit
// `CALL(bar, 1)` at verify-off.
static int foo_body(int x) { return x; }
static int bar_body(int x) { return x + 1; }
#define impl_foo(x) foo_body(x)
#define impl_bar(x) bar_body(x)
#define CAT(a, b) a##b
#define CALL(n, v) int n##_tag = CAT(impl_, n)(v);
CALL(foo, 1)
