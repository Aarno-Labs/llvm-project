// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FIRST
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 SECOND
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 THIRD
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FOURTH
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FIFTH
// test16.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test16.c -o test16.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T16:IF-GROUP:BEGIN*/
#if 1
/*BOUNDARY:T16:IF-ARM:BEGIN*/
#include "h1.h"
#include "h6_defs_undefs.h"
RF_MARK(t16_inside_if_after_includes)
/*BOUNDARY:T16:IF-ARM:END*/
#else
RF_PAYLOAD(t16_else)
#endif
/*BOUNDARY:T16:IF-GROUP:END*/
RF_MARK(t16_after)

int __refold_ins__t16_after = 0; /* pure insertion at after boundary for t16_after_marker */
int main(void) {
  return 0;
}
