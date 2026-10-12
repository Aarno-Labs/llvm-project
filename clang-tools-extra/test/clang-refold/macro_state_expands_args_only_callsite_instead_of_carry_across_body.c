// RUN: %clang-refold-tester-verify-off macro_state_expands_args_only_callsite_instead_of_carry_across_body
// Regression: carrying `#define X` past the args-only `M(X + 1)` crossed the
// callsite it keeps, and `M`'s body reads `X` through `Y`, so `Y` became the
// enumerator.  The carry judged the callsite by its text, which never spells
// `X`; it now also asks the producer, refuses, and `M` is expanded instead.
enum { X = 3 };
#define Y X
#define M(a) int k = a + Y;
#define X 5
M(1)
