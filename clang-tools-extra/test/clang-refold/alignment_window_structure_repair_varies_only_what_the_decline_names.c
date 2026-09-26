// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=0 %clang-refold-tester alignment_window_structure_repair_varies_only_what_the_decline_names
// RUN: FileCheck --input-file=%t/outputs/alignment_window_structure_repair_varies_only_what_the_decline_names.out %s
//
// Regression: when the structure-respecting repair's combinations exceed the
// realization budget, it varies only the sub-rectangles that the decline's
// own terminal requests name, and leaves the rest at core-forced anchors.
//
// Two deletions share one certification window.  Deleting `static int\nx;`
// between the `#define` lines is the shape of
// alignment_window_commits_structure_respecting_repair_when_decline_fails.c:
// declining it spans both directives and requests terminal fallback, and the
// keys keep the one map that deletes exactly the declaration.  Deleting one
// `1,` line from `t` gives five optimal maps.  The keys keep the three that
// delete a whole line, and declining that sub-rectangle is accepted.
//
// Three combinations plus the decline exceed the injected budget, so before
// this fix the repair declined and the run reached the whole-translation-unit
// carrier.  Only the declaration's sub-rectangle meets the decline's terminal
// request, so one combination is realized, accepted, and committed.
//
// Regression for file's `der.c`, where a deleted `der__tag` table between
// der.h's last declaration and `#ifdef DEBUG_DER` shared a window with an
// unrelated ambiguity that realized cleanly.
//
// CHECK: window 0: sub-rectangle A=[2,9) B=[2,5) keeps 1 of its 4 optimal map(s) under the structure-respecting keys
// CHECK: window 0: sub-rectangle A=[17,23) B=[13,17) keeps 3 of its 5 optimal map(s) under the structure-respecting keys
// CHECK: window 0: its 2 preferred sub-rectangle(s) combine into more maps than the realization budget admits; varying only the 1 its core-forced alignment's terminal request(s) meet
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback; 1 preferred sub-rectangle(s) combine into 1 map(s)
int y;
#define A
static int
x;
#define B
static int z;
int t[] = {
  1,
  1,
  1,
  2 };
