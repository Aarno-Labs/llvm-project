// RUN: %clang-refold-tester vaopt_empty_to_nonempty_arg_inversion
// Regression (was a miscompile): when __VA_OPT__ toggles from empty to
// non-empty, the __VA_OPT__(,)-introduced comma must not be folded into the
// invocation as literal argument text. Editing the expansion "1 " -> "1 , 2"
// refolds to M2(1, 2); the tool previously emitted the malformed M2(1, , 2).
#define M2(a, ...) a __VA_OPT__(,) __VA_ARGS__
int x[] = {M2(1, 2)};
