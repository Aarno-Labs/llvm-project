// RUN: %clang-refold-tester-verify-off macro_state_undef_before_args_only_actual_naming_macro
// Regression: B inserts the enumerator `X` into the actual of `M`.  The
// args-only rewrite `M(X + 1)` was never audited for macro names, so `X`
// expanded to 5 where B has the enumerator 3.  The certified payload now puts
// it under the `#undef X` partition.  The enumerator keeps B valid C.
enum { X = 3 };
#define X 5
#define M(a) int k = a;
M(1)
