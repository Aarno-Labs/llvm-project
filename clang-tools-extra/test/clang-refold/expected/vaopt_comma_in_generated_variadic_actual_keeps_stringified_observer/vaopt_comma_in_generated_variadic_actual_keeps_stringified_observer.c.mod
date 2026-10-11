// RUN: %clang-refold-tester-verify-off vaopt_comma_in_generated_variadic_actual_keeps_stringified_observer
// A `__VA_OPT__` group's `,` inside a generated call's variadic actual is that
// actual's content, not a boundary, so the actual depends on the group.
//
// `FWD` spells `G(X __VA_OPT__(, __VA_ARGS__))`, and `ALLS` has only a
// variadic formal, so the group's output `, 10` stays in `ALLS`'s one actual
// and is stringified as `"alpha , 10"`.  B renames only the direct use
// `int k = 10;`; rewriting `APPLY`'s variadic actual to `20` would also
// re-spell the unchanged literal as `"alpha , 20"`, so the invocation is
// expanded.
#define ALLS(...) #__VA_ARGS__
#define FW(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__))
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__); int k = __VA_ARGS__
const char *s = "alpha , 10"; int k = 20;
