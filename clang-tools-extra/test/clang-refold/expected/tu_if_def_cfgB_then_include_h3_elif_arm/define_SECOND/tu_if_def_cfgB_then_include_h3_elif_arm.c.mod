// RUN: %clang-refold-tester-with-lines tu_if_def_cfgB_then_include_h3_elif_arm FIRST
// RUN: %clang-refold-tester-with-lines tu_if_def_cfgB_then_include_h3_elif_arm SECOND
// test25.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test25.c -o test25.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T25:OUTER-IF:BEGIN*/
#if 1
#define RF_CFG_B 1
/*BOUNDARY:T25:IF-ARM:INCLUDE-H3:BEGIN*/
#include "h3_conditional.h"
/*BOUNDARY:T25:IF-ARM:INCLUDE-H3:END*/
#endif
/*BOUNDARY:T25:OUTER-IF:END*/
RF_MARK(t25_after)

int __refold_ins__t25_after = 0; /* pure insertion at after boundary for t25_after_marker */
#line 18 "tu_if_def_cfgB_then_include_h3_elif_arm.c"
int main(void) {
  return 0;
}
