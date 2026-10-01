// RUN: %clang-refold-tester alignment_window_plain_sub_rectangles_held_at_envelope
// RUN: FileCheck --input-file=%t/outputs/alignment_window_plain_sub_rectangles_held_at_envelope.out %s
//
// Regression: plain repeated-token ambiguity must not price a window's real
// ambiguity out of resolution.
//
// The counter edits are those of
// semantic_alignment_counter_argument_growth_two_windows.c: 54 optimal maps,
// of which the least-source-mutation rule keeps every LEFT_COUNTED and
// RIGHT_COUNTED invocation.  B also inserts `int plain_c = 1;` and
// `int plain_d = 2;` after the two plain declarations.  Each insertion repeats
// the `= 1 ;` or `= 2 ;` before it, so each is a sub-rectangle with 5 optimal
// maps, and the window's ground set grows to 54 * 5 * 5 = 1350, past the proof
// budget.  Before this fix the window could not be enumerated, every rule
// declined, and the refold expanded five of the six counted invocations.
//
// Both sub-rectangles are plain source -- direct spellings separated by
// whitespace alone -- so their maps can differ in whitespace only.  Held at
// their forced-only envelope they contribute one map each, the window
// enumerates the 54 counter maps, and the least-source-mutation rule commits
// as it does without the plain insertions.
//
// CHECK: window 0: sub-rectangle A=[{{[0-9]+}},{{[0-9]+}}) B=[{{[0-9]+}},{{[0-9]+}}) is plain source; held at its forced-only envelope
// CHECK: window 0: sub-rectangle A=[{{[0-9]+}},{{[0-9]+}}) B=[{{[0-9]+}},{{[0-9]+}}) is plain source; held at its forced-only envelope
// CHECK: window 0 carries alignment ambiguity over A=[{{[0-9]+}},{{[0-9]+}}) B=[{{[0-9]+}},{{[0-9]+}}): 54 static realization class representative(s) enumerated; 2 plain sub-rectangle(s) held at their forced-only envelope, 0 reduced to their classes
// CHECK: window 0 committed CompleteGroundSetEquivalence: 1 of 54 static-class representative map(s) share its output

#define ADD(x, y) ((x) + (y))
#define LEFT_COUNTED(x) ADD(x, __COUNTER__)
#define RIGHT_COUNTED(x) ADD(__COUNTER__, x)
int left_first = LEFT_COUNTED(1);
int left_middle = LEFT_COUNTED((6) + 4);
int left_last = LEFT_COUNTED(11);
int plain_a = 1;
int plain_c = 1;
int plain_b = 2;
int plain_d = 2;
int semantic_window_separator = 909;
int right_first = RIGHT_COUNTED(1);
int right_middle = RIGHT_COUNTED((6) + 4);
int right_last = RIGHT_COUNTED(11);
