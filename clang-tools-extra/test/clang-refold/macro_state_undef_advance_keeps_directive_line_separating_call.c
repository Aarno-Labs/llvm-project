// RUN: %clang-refold-tester-verify-off macro_state_undef_advance_keeps_directive_line_separating_call
// Regression: B's payload names `X`, and the repair advanced the preserved
// `#undef X` before it.  That line stood between `F` and `(y)` and was all
// that kept `F` from calling.  A directive line is no longer moved when the
// tokens on either side of it would join a function-like name to `(`; the
// partition repairs the payload instead.
typedef int F;
enum { X = 5 };
#define X 1
#define F(v) v
int b = X;
int a = 0; F
#undef X
(y);
int z = X;
