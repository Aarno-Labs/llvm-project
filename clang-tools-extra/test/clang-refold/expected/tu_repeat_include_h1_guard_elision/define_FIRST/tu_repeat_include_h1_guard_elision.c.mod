// RUN: %clang-refold-tester-with-lines tu_repeat_include_h1_guard_elision FIRST
// RUN: %clang-refold-tester-with-lines tu_repeat_include_h1_guard_elision SECOND
// test17.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test17.c -o test17.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T17:REPEAT-INCLUDE-H1:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#line 9 "tu_repeat_include_h1_guard_elision.c"
#include "h1.h"
#include "h1.h" // should be elided by guard
/*BOUNDARY:T17:REPEAT-INCLUDE-H1:END*/
RF_MARK(t17_after)

int main(void) {
  return 0;
}
