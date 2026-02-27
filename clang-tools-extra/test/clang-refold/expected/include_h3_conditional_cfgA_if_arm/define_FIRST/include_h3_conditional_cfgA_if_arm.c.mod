// RUN: %clang-refold-tester-with-lines include_h3_conditional_cfgA_if_arm FIRST
// RUN: %clang-refold-tester-with-lines include_h3_conditional_cfgA_if_arm SECOND
// test04.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test04.c -o test04.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_CFG_A 1
/*BOUNDARY:T04:INCLUDE-H3:BEGIN*/
int __refold_ins__h3_if_begin = 0; /* pure insertion at before boundary for h3_if_begin_marker */
#line 10 "include_h3_conditional_cfgA_if_arm.c"
#include "h3_conditional.h"
/*BOUNDARY:T04:INCLUDE-H3:END*/
RF_MARK(t04_after)

int main(void) {
  return 0;
}
