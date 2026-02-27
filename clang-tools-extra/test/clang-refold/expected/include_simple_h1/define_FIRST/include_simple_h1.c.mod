// RUN: %clang-refold-tester-with-lines include_simple_h1 FIRST
// RUN: %clang-refold-tester-with-lines include_simple_h1 SECOND
// RUN: %clang-refold-tester-with-lines include_simple_h1 THIRD
// RUN: %clang-refold-tester-with-lines include_simple_h1 TIEBREAK
// Preprocess: clang -E -P -I headers test01.c -o test01.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T01:INCLUDE-H1:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#line 10 "include_simple_h1.c"
#include "h1.h"
/*BOUNDARY:T01:INCLUDE-H1:END*/

RF_MARK(t01_after)

int main(void) {
  return 0;
}
