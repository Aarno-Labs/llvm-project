// RUN: %clang-refold-tester-verify-off header_sideband_pragma_deletion_opening_call_realizes_include_from_b
// Regression: B deletes the header's `#pragma` line between `F` and `(y)`.
// The materialized header dropped the line and called `F`.  Deleting a
// sideband site in a header now reads the seam it leaves, and the include is
// realized from B, where the translation unit's partition repairs it.
typedef int F;
#define F(v) v
int r = F(1);
#undef F
F
(y);
#pragma GCC diagnostic pop
int t = 0;
#define F(v) v
int w = F(3);
