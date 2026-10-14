// RUN: %clang-refold-tester-verify-off macro_state_carry_keeps_directive_line_separating_call
// Regression: B's payload names `X`, and the carry moved `#define X 1` past
// it.  That line stood between `F` and `(y)`, which A shows adjacent, and
// was all that kept `F` from calling.  A directive line is no longer moved
// when the tokens on either side of it would join a function-like name to
// `(`; the partition repairs the payload instead.
typedef int F;
enum { X = 5 };
#define F(v) v
F
#define X 1
(y); int a = 0;
int z;
