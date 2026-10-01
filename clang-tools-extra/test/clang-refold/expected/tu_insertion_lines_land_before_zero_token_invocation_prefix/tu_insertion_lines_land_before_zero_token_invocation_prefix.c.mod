// RUN: %clang-refold-tester tu_insertion_lines_land_before_zero_token_invocation_prefix
// RUN: FileCheck --input-file=%t/outputs/tu_insertion_lines_land_before_zero_token_invocation_prefix.out %s
//
// Regression: inserted whole lines land before the zero-token macro
// invocations that begin the line they precede, not between those invocations
// and the line's first token.
//
// `NOTE(...)` expands to nothing, so in f the A tokens around the insertion
// are `{` and `long`, and the pure insertion is anchored at `long`.  B prints
// `int added = 5;` as a line of its own before the line holding `long kept`.
// Before this fix the edit was spliced in at the anchor, after `NOTE(...)`,
// which left `long kept = 1;` alone on the following line.  The line's prefix
// holds only a source-neutral zero-token invocation and whitespace, so moving
// the insertion to the start of the line emits the same tokens, and the new
// line now precedes the NOTE line.
//
// In g, B prints the insertion on the same line as `long kept_inline`.  It is
// not whole lines, so it stays at its anchor.
//
// Regression for speex's lsp.c, where `VARDECL(float *x_freq);` began the line
// that five inserted declarations preceded.
//
// CHECK: insertion A=6 B=[6,11) placed at source byte {{[0-9]+}} (base anchor {{[0-9]+}}), before the zero-token macro invocations that begin its line
// CHECK-NOT: insertion A=18 {{.*}}before the zero-token macro invocations

#define NOTE(text)
void f(void) {
    int added = 5;
    NOTE(the declaration below is kept) long kept = 1;
}
void g(void) {
    NOTE(the declaration below is kept) int added_inline = 6; long kept_inline = 2;
}
