// RUN: %clang-refold-tester-with-lines tu_if_else_include_in_if_arm FIRST
// RUN: %clang-refold-tester-with-lines tu_if_else_include_in_if_arm SECOND
// test03.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test03.c -o test03.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T03:IF-GROUP:BEGIN*/
#if 1
/*BOUNDARY:T03:IF-ARM:INCLUDE-H1:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#include "h1.h"
/*BOUNDARY:T03:IF-ARM:INCLUDE-H1:END*/
#else
/*BOUNDARY:T03:ELSE-ARM:BEGIN*/
RF_PAYLOAD(t03_else)
/*BOUNDARY:T03:ELSE-ARM:END*/
#endif
/*BOUNDARY:T03:IF-GROUP:END*/

#line 20 "tu_if_else_include_in_if_arm.c"
RF_MARK(t03_after)

int main(void) {
  return 0;
}
