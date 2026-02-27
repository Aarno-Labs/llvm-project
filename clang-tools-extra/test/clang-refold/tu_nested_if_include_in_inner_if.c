// RUN: %clang-refold-tester-with-lines tu_nested_if_include_in_inner_if FIRST
// RUN: %clang-refold-tester-with-lines tu_nested_if_include_in_inner_if SECOND
// test09.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test09.c -o test09.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T09:OUTER-IF:BEGIN*/
#if 1
/*BOUNDARY:T09:INNER-IF:BEGIN*/
#if 1
/*BOUNDARY:T09:INNER-IF:INCLUDE-H1:BEGIN*/
#include "h1.h"
/*BOUNDARY:T09:INNER-IF:INCLUDE-H1:END*/
#else
RF_PAYLOAD(t09_inner_else)
#endif
/*BOUNDARY:T09:INNER-IF:END*/
#else
RF_PAYLOAD(t09_outer_else)
#endif
/*BOUNDARY:T09:OUTER-IF:END*/

RF_MARK(t09_after)

int main(void) {
  return 0;
}
