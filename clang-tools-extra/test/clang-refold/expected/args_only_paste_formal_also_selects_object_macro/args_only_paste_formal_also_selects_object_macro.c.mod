// RUN: %clang-refold-tester-verify-off args_only_paste_formal_also_selects_object_macro
// An args-only rewrite must not change an object-like macro that the
// rewritten formal names by pasting.
//
// `n` is pasted into `n##_tag` and into `SEL_##n`, which expands the macro
// `SEL_foo`.  B renames only the tag, so rewriting the actual to `bar` would
// also expand `SEL_bar` and turn the unchanged `foo_value` into `bar_value`.
// The rewrite is refused and the invocation is expanded.  This used to emit
// `CALL(bar)` at verify-off.
static const int foo_value = 1;
static const int bar_value = 2;
#define SEL_foo foo_value
#define SEL_bar bar_value
#define CALL(n) int n##_tag = SEL_##n;
int bar_tag = foo_value;
