// RUN: %clang-refold-tester-verify-off macro_state_whole_cover_carry_keeps_directive_line_separating_call
// Regression: B rewrites `M()` as `X`, and the carry that runs after the
// whole cover is staged moved `#define X 1` past it.  That line stood
// between `F` and `(y)` and was all that kept `F` from calling.  A directive
// line is no longer moved when the tokens on either side of it would join a
// function-like name to `(`; the partition repairs the payload instead.
typedef int F;
enum { X = 5 };
#define F(v) v
#define M() 7
F
#define X 1
#undef X
(y); int a = X;
int z;
