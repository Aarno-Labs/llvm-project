// RUN: %clang-refold-tester alignment_window_comment_sub_rectangle_not_held_at_envelope
// RUN: FileCheck --input-file=%t/outputs/alignment_window_comment_sub_rectangle_not_held_at_envelope.out %s
//
// A sub-rectangle is held at its forced-only envelope only when it is plain
// source.  This is alignment_window_plain_sub_rectangles_held_at_envelope.c
// with a comment after `int plain_a = 1;`.  The `int plain_c = 1;` insertion's
// maps now differ in which side of that comment the new declaration lands, so
// they are not interchangeable and that sub-rectangle is still enumerated.
// Only the `plain_b` one is held, the window's ground set is 54 * 5 = 270, past
// the proof budget, and the window keeps its core-forced anchors.
//
// CHECK: is plain source; held at its forced-only envelope
// CHECK-NOT: is plain source; held at its forced-only envelope
// CHECK: window 0 keeps its core-forced anchors: its optimal maps could not be enumerated within the proof budget

#define ADD(x, y) ((x) + (y))
#define LEFT_COUNTED(x) ADD(x, __COUNTER__)
#define RIGHT_COUNTED(x) ADD(__COUNTER__, x)
int left_first = LEFT_COUNTED(1);
int left_middle = (((6) + 4) + (1));
int left_last = ((11) + (2));
int plain_a = 1;
int plain_c = 1; /* the comment */
int plain_b = 2;
int plain_d = 2;
int semantic_window_separator = 909;
int right_first = ((3) + (1));
int right_middle = ((4) + ((6) + 4));
int right_last = ((5) + (11));
