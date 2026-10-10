// RUN: %clang-refold-tester-verify-off whole_cover_replaces_source_closing_generated_call_edited_output
// Same consumed `)` as whole_cover_replaces_source_closing_generated_call.c,
// with the edit inserted at the end of the generated call's output instead.
// The realization used to emit `int x; int g_x = 1);` at verify-off.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, x));
