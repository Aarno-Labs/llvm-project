// RUN: %clang-refold-tester-with-lines include_header_empty_if_arm_else_payload
// RUN: %clang-refold-tester-with-lines include_header_empty_if_arm_else_payload TIEBREAK
// Preprocess: clang -E -P -I headers test15.c -o test15.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T15:INCLUDE-H7:BEGIN*/
#include "h7_empty_arm.h"
/*BOUNDARY:T15:INCLUDE-H7:END*/
RF_MARK(t15_after)

int __refold_ins__t15_after = 0; /* pure insertion at after boundary for t15_after_marker */
#line 12 "include_header_empty_if_arm_else_payload.c"
int main(void) {
  return 0;
}
