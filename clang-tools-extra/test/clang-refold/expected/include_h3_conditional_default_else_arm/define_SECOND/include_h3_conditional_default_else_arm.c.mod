// RUN: %clang-refold-tester-with-lines include_h3_conditional_default_else_arm FIRST
// RUN: %clang-refold-tester-with-lines include_h3_conditional_default_else_arm SECOND
// test05.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test05.c -o test05.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T05:INCLUDE-H3:BEGIN*/
#include "h3_conditional.h"
/*BOUNDARY:T05:INCLUDE-H3:END*/
RF_MARK(t05_after)

int __refold_ins__t05_after = 0; /* pure insertion at after boundary for t05_after_marker */
#line 13 "include_h3_conditional_default_else_arm.c"
int main(void) {
  return 0;
}
