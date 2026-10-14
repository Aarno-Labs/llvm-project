// RUN: %clang-refold-tester-verify-off materialized_header_boundary_opening_call_realizes_include_from_b
// Regression: `F` ends the line before an `#include` whose header begins
// with `(y);`.  Clang forms no call across the directive line, but
// materializing the header for its own edit deleted that line, so the copied
// text called `F`.  Inlining now reads the seams it makes at the include's
// boundaries, and the include is realized from B, where the translation
// unit's partition repairs it.
typedef int F;
#define F(v) v
int r = F(1);
F
#undef F
(y);
int q = 2;
int t = 0;
#define F(v) v
int w = F(3);
