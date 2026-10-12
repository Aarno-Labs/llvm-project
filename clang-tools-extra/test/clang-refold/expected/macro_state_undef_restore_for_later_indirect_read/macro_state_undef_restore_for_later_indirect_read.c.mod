// RUN: %clang-refold-tester-verify-off macro_state_undef_restore_for_later_indirect_read
// Regression: the synthetic `#undef X` placed before B's `X` must be followed
// by a restore when source after the replacement needs the definition, and a
// later line can need it without spelling it.  `int j = Y;` reads `X` through
// `Y`, so leaving `X` undefined made `j` the enumerator 3 where B has 5.  The
// restore decision scanned the suffix's text for `X` alone; it now also asks
// whether the producer recorded an expansion of `X` there.  The enumerator
// keeps the edited stream valid C.
enum { X = 3 };
#define X 5
#define Y X
#undef X
int k = X;
#define X 5
int j = Y;
