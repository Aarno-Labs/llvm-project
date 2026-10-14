// RUN: %clang-refold-tester-verify-off materialized_header_macro_patch_brackets_instead_of_moving_call_separator
// Regression: B rewrites the header's `M()` as `X`, and the header patch
// stabilizer carried `#define X 1` past it.  That line stood between `F` and
// `(y)` and was all that kept `F` from calling.  A directive line is no
// longer moved when the tokens on either side of it would join a
// function-like name to `(`; the stabilizer brackets the payload instead.
typedef int F;
enum { X = 5 };
#define F(v) v
#define M() 7
F
#define X 1
#undef X
(y); int a = X;
#define X 1
int z;
int w = 3;
