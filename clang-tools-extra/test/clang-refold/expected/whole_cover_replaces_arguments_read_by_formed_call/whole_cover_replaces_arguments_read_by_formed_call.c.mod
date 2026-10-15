// RUN: %clang-refold-tester-verify-off whole_cover_replaces_arguments_read_by_formed_call
// Regression: `GET(TW)` expands to `DO_TW`, which reads `(10)` from the source
// after the callsite.  No source spells DO_TW's name, so the producer records
// no `inv_e` for it, and the whole cover realizing B's `thrice(10)` replaced
// only `GET(TW)`, leaving `(10)` behind it.  The cover now ends at the latest
// use end (`use_e`) among the calls its expansion formed.
int twice(int v) { return 2 * v; }
int thrice(int v) { return 3 * v; }
#define GET(k) DO_##k
#define DO_TW(x) twice(x)
int a = thrice(10);
