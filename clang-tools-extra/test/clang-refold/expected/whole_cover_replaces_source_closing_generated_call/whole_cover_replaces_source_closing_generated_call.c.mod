// RUN: %clang-refold-tester-verify-off whole_cover_replaces_source_closing_generated_call
// A whole-cover realization replaces every source byte its expansion
// consumed, not only the invocation's own callsite.
//
// `CALL` leaves `G(x` open, and the second `)` after `CALL(G, x)` closes it.
// B renames only `int x`, which `G`'s paste also observes, so the invocation is
// realized from B.  The realization used to replace `CALL(G, x)` alone and
// leave the consumed `)` behind, emitting `int y; int g_x);` at verify-off.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
int y; int g_x;
