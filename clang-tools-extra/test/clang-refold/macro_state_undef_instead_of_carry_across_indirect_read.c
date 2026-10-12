// RUN: %clang-refold-tester-verify-off macro_state_undef_instead_of_carry_across_indirect_read
// Regression: `#define X` may be carried past B's `X` only when no preserved
// source it crosses needs the definition.  `int j = Y;` reads `X` through `Y`
// without spelling it, so carrying the definition made `j` the enumerator 3
// where B has 5.  The crossing check scanned text alone; it now also asks
// whether the producer recorded an expansion of `X` there, so the carry is
// refused and `X` is undefined before the replacement's line instead.  The
// enumerator keeps the edited stream valid C.
enum { X = 3 };
#define Y X
#define X 5
int j = Y;
int k = 0;
