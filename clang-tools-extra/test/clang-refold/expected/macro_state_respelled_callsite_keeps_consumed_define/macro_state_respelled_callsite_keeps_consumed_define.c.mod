// RUN: %clang-refold-tester-verify-off macro_state_respelled_callsite_keeps_consumed_define
// Regression: the first edit consumes `#define Z 5`.  The `#undef X`
// partition for the second payload's painted `X` widens the second edit over
// its line and re-spells `int k = W;`, whose expansion read `Z`.  That
// callsite counted as consumed by the widened edit, and the only other
// reader, the edited `int v`, was replaced, so the definition was dropped and
// `W` stayed `Z`.  A re-spelled callsite now survives, and the definition is
// replayed for it.
int X = 1;
int Z = 3;
#define X (X + 1)
#define W Z
#undef X
int j = Z; int arr[] = { X
#define Z 5
 };
#undef X
int k = W; int arr2[] = { X };
int v = 6;
