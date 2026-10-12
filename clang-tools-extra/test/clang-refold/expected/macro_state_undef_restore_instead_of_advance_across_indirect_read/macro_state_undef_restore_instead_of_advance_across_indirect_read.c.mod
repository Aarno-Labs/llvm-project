// RUN: %clang-refold-tester-verify-off macro_state_undef_restore_instead_of_advance_across_indirect_read
// Regression: a later `#undef X` may be advanced before B's `X` only when the
// source it moves across does not need the definition.  `int j = Y;` lies
// between them and reads `X` through `Y`, so advancing the `#undef` made `j`
// the enumerator 3 where B has 5.  The crossing check scanned text alone; it
// now also asks whether the producer recorded an expansion of `X` there, so
// the advance is refused and the replacement's line is bracketed by an
// `#undef`/restore pair instead.  The enumerator keeps the edited stream
// valid C.
enum { X = 3 };
#define X 5
#define Y X
#undef X
int k = X;
#define X 5
int j = Y;
#undef X
