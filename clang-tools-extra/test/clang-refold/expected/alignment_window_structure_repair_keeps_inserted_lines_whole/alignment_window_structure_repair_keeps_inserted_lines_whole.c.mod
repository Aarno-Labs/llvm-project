// RUN: %clang-refold-tester alignment_window_structure_repair_keeps_inserted_lines_whole
// RUN: FileCheck --input-file=%t/outputs/alignment_window_structure_repair_keeps_inserted_lines_whole.out %s
//
// Regression: when declining an ambiguous window is inadmissible, the
// structure-respecting repair places a pure insertion as whole lines of B.
//
// B inserts two `int *_index_xj` declarations after `*n4=NULL;`.  Their
// repeated `int`, `=`, `0` and `;` give the insertion 13 optimal maps, which
// keep every A token and differ only in where the new tokens go.  None removes
// anything, so the repair's four removal keys tie, and before this fix the
// sub-rectangle stayed at its forced-only envelope.  That envelope replaces
// source holding the zero-token `VARDECL(float *x_freq)`, which no edit can
// cover, so the repair's one combination requested terminal fallback and the
// run failed.  Deleting `static int\nx;` between the two `#define` lines has
// the shape of
// alignment_window_commits_structure_respecting_repair_when_decline_fails.c:
// it makes every earlier rule decline and the decline itself inadmissible.
//
// The fifth key keeps the one map whose inserted tokens are whole lines of B,
// the two declarations, and the window commits it.
//
// The declarations then land before `VARDECL(float *x_freq);`, as in
// tu_insertion_lines_land_before_zero_token_invocation_prefix.c.
//
// Regression for speex's lsp.c.
//
// CHECK: window 0: sub-rectangle A=[2,9) B=[2,5) keeps 1 of its 4 optimal map(s) under the structure-respecting keys
// CHECK: window 0: sub-rectangle A=[23,28) B=[19,35) keeps 1 of its 13 optimal map(s) under the structure-respecting keys
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback; 2 preferred sub-rectangle(s) combine into 1 map(s)

#define NULL 0
#define VARDECL(var)
int y;
#define A

#define B
static int z;
void f(void) {
    float *n3,*n4=NULL;
    int n3_index_xj = 0;
    int n4_index_xj = -1;
    VARDECL(float *x_freq);
    int m = 1;
}
