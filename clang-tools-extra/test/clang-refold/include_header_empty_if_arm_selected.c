// RUN: %clang-refold-tester-with-lines include_header_empty_if_arm_selected

// test14.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test14.c -o test14.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_EMPTY_ARM 1
/*BOUNDARY:T14:INCLUDE-H7:BEGIN*/
#include "h7_empty_arm.h"
/*BOUNDARY:T14:INCLUDE-H7:END*/
RF_MARK(t14_after)

int main(void) {
  return 0;
}
