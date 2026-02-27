// RUN: %clang-refold-tester-with-lines tu_if_elif_else_include_in_else_arm FIRST
// RUN: %clang-refold-tester-with-lines tu_if_elif_else_include_in_else_arm SECOND
// test08.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test08.c -o test08.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T08:IF-GROUP:BEGIN*/
#if defined(T08_A)
/*BOUNDARY:T08:IF-ARM:BEGIN*/
RF_PAYLOAD(t08_if)
/*BOUNDARY:T08:IF-ARM:END*/
#elif defined(T08_B)
/*BOUNDARY:T08:ELIF-ARM:BEGIN*/
RF_PAYLOAD(t08_elif)
/*BOUNDARY:T08:ELIF-ARM:END*/
#else
/*BOUNDARY:T08:ELSE-ARM:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#line 19 "tu_if_elif_else_include_in_else_arm.c"
#include "h1.h"
/*BOUNDARY:T08:ELSE-ARM:END*/
#endif
/*BOUNDARY:T08:IF-GROUP:END*/

RF_MARK(t08_after)

int main(void) {
  return 0;
}
