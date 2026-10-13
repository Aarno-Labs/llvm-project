// RUN: %clang-refold-tester-verify-off macro_state_replayed_define_after_respelled_indirect_reader
// Regression: the edit replacing `11, ... 22` consumes `#define Z 5`, which
// the later `Z` needs.  The payload's `X` is bound to a painted definition,
// so the `#undef X` partition widens the edit over its line and re-spells
// `int j = W;`, which read `Z` unbound through `W`.  Replaying the definition
// before the replacement made that `W` expand to `5`; the text scan saw no
// `Z`.  The replay now places it after the re-spelled reader.
int X = 1;
int Z = 3;
#define X (X + 1)
#define W Z
int j = W; int arr[] = { 11,
#define Z 5
22 };
int v = Z;
