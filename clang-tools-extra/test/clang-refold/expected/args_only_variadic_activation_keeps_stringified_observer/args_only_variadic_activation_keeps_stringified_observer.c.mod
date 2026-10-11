// RUN: %clang-refold-tester-verify-off args_only_variadic_activation_keeps_stringified_observer
// Supplying an omitted variadic actual must not leave the root's own
// stringification of it stale.
//
// B appends `, 7` through the `__VA_OPT__` tail but keeps the literal `""`
// that `#__VA_ARGS__` spells.  Rewriting the invocation to `CALL(G, 7)`
// would re-spell that literal as `"7"`, so that fold is refused, and the
// appended tokens are inserted in source after the invocation, which keeps it
// and its literal.  This used to emit `CALL(G, 7)` at verify-off.
static int g(int x) { return x; }
#define G(x) g(x)
#define CALL(f, ...) const char *s = #__VA_ARGS__; int v[] = { f(0) __VA_OPT__(, __VA_ARGS__)
CALL(G), 7 };
