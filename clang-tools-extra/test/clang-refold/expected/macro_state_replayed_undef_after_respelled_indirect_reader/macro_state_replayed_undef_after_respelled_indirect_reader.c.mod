// RUN: %clang-refold-tester-verify-off macro_state_replayed_undef_after_respelled_indirect_reader
// Regression: the edit replacing `11, ... 22` consumes `#undef Z`, which the
// later `Z` needs.  The `#undef X` partition for the payload's painted `X`
// widens the edit over its line and re-spells `int j = W;`, which read `Z`'s
// definition through `W`.  Replaying the `#undef` before the replacement left
// that `W` as `Z`.  The replay now places it after the re-spelled reader.
int X = 1;
int Z = 3;
#define X (X + 1)
#define W Z
#define Z 5
#undef X
int j = W; int arr[] = { X };
#undef Z
int v = Z;
