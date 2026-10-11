// RUN: %clang-refold-tester-verify-off generated_variadic_actual_shifted_by_forwarded_comma_keeps_stringified_observer
// A variadic actual forwarded ahead of a generated variadic call's later
// actuals can move their boundaries, so those actuals depend on it.
//
// `FW2` spells `G(__VA_ARGS__, tail)`.  With the variadic actual `1`,
// `REST`'s variadic actual is `tail`, stringified as `"tail"`; with `1, 2`
// it would be `2, tail`.  `tail` is replacement-list text, so only the
// producer's rule for a reference at the top level before an actual makes
// `REST`'s variadic actual depend on `FW2`'s variadic formal.  B renames only
// the direct use `{1}`; rewriting `APPLY`'s variadic actual to `1, 2` would
// re-spell the unchanged literal as `"2, tail"`, so the invocation is
// expanded.
#define REST(a, ...) #__VA_ARGS__
#define FW2(G, ...) G(__VA_ARGS__, tail)
#define APPLY(F, G, ...) const char *s = F(G, __VA_ARGS__); int arr[] = {__VA_ARGS__};
const char *s = "tail"; int arr[] = {1, 2};
