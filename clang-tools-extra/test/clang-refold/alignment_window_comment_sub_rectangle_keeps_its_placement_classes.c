// RUN: %clang-refold-tester alignment_window_comment_sub_rectangle_keeps_its_placement_classes
// RUN: FileCheck --input-file=%t/outputs/alignment_window_comment_sub_rectangle_keeps_its_placement_classes.out %s
//
// A comment makes a sub-rectangle's alignments distinguishable, and static
// realization classes keep exactly that distinction.
//
// This is alignment_window_plain_sub_rectangles_held_at_envelope.c with a
// comment after `int plain_a = 1;`.  The `int plain_c = 1;` insertion is then
// not plain, and its five optimal maps fall into three classes by what they
// do around the comment: insert after it, insert into its gap, or insert
// before the `;` that precedes it.  The second class stays apart from the first
// because which side of the comment an insertion into its gap takes is the
// realizer's choice.  The `plain_b` sub-rectangle is still plain and held, so
// the window enumerates 9 * 3 * 6 = 162 representatives instead of 1350 maps.
//
// The placements keep the same constructs, so least source mutation cannot
// rank them and no rule commits; the window keeps its core-forced anchors.
// Choosing between such placements is a layout rule this does not provide.
//
// CHECK: 5 optimal map(s) in 3 static realization class(es)
// CHECK: is plain source; held at its forced-only envelope
// CHECK: 162 static realization class representative(s) enumerated; 1 plain sub-rectangle(s) held at their forced-only envelope, 1 reduced to their classes
// CHECK: window 0 keeps its core-forced anchors: no commit rule proved one admissible realization among its 162 enumerated map(s)

#define ADD(x, y) ((x) + (y))
#define LEFT_COUNTED(x) ADD(x, __COUNTER__)
#define RIGHT_COUNTED(x) ADD(__COUNTER__, x)
int left_first = LEFT_COUNTED(0);
int left_middle = LEFT_COUNTED(5);
int left_last = LEFT_COUNTED(10);
int plain_a = 1; /* the comment */
int plain_b = 2;
int semantic_window_separator = 909;
int right_first = RIGHT_COUNTED(0);
int right_middle = RIGHT_COUNTED(5);
int right_last = RIGHT_COUNTED(10);
