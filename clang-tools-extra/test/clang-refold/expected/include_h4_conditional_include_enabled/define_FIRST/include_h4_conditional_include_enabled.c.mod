// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled FIRST
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled SECOND
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled THIRD
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled FOURTH
// test06.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test06.c -o test06.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_ENABLE_EXTRA 1
/*BOUNDARY:T06:INCLUDE-H4:BEGIN*/
int __refold_ins__h4_begin = 0; /* pure insertion at before boundary for h4_begin_marker */
#line 12 "include_h4_conditional_include_enabled.c"
#include "h4_conditional_include.h"
/*BOUNDARY:T06:INCLUDE-H4:END*/
RF_MARK(t06_after)

int main(void) {
  return 0;
}
