// RUN: %clang-refold-tester alignment_window_commits_structure_respecting_repair_when_decline_fails
// RUN: FileCheck --input-file=%t/outputs/alignment_window_commits_structure_respecting_repair_when_decline_fails.out %s
//
// Regression: when declining an ambiguous window is itself inadmissible, the
// resolver commits the candidate whose hunks leave protected structure alone.
//
// The edit deletes the two-line declaration of `x`.  As with a one-line
// declaration, the repeated `;` and `static int` give four optimal alignments,
// and every one of them is accepted.  They are the same deletion slid by one
// token, so none changes a subset of another's source and the containment
// rule finds no least element; the window declines and keeps core-forced
// anchors, whose single hunk spans both `#define` lines and requests terminal
// fallback.  The line-aligned narrowing cannot help: every narrowing of that
// hunk removes a newline.
//
// Three of the four maps straddle a `#define`.  The one that deletes exactly
// `static int\nx;` straddles neither, so the first structure-respecting key
// keeps only it; its realization is accepted and the decline's is not, so the
// window commits it.
//
// CHECK: window 0 has no unique least source-mutation class: accepted=4 leastDestructive=0
// CHECK: window 0: sub-rectangle A=[2,9) B=[2,5) keeps 1 of its 4 optimal map(s) under the structure-respecting keys
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback; 1 preferred sub-rectangle(s) combine into 1 map(s)
int y;
#define A

#define B
static int z;
