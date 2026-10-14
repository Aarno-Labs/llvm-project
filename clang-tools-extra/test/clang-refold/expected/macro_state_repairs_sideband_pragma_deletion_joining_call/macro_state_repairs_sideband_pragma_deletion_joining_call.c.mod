// RUN: %clang-refold-tester-verify-off macro_state_repairs_sideband_pragma_deletion_joining_call
// Regression: B deletes the `#pragma` line between `F` and `(y)`.  The line
// was all that kept `F` from calling, and the sideband deletion certified no
// B position, so nothing read the seam it leaves and the output called `F`.
// A pragma deletion now marks that seam like any other deletion.
typedef int F;
#define F(v) v
int r = F(1);
F
#undef F
(y);
#define F(v) v
#pragma GCC diagnostic pop
