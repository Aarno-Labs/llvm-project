// RUN: %clang-refold-tester-verify-off whole_cover_replaces_source_closing_generated_call_edited_output
// Same consumed `)` as whole_cover_replaces_source_closing_generated_call.c,
// with the edit on the generated call's output instead: B renames `g_x`, which
// only an expansion realizes.  The realization used to emit `int x; int h_x);`
// at verify-off.  An insertion after `g_x` should fold after the consumed `)`,
// which generated_call_output_insertion_after_consumed_source_refolds pins.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
int x; int h_x;
