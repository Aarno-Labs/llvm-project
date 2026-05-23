// RUN: %clang-refold-tester-with-lines include_simple_h1 FIRST
// RUN: %clang-refold-tester-with-lines include_simple_h1 SECOND
// RUN: %clang-refold-tester-with-lines include_simple_h1 THIRD
// RUN: %clang-refold-tester-with-lines include_simple_h1 TIEBREAK
// Preprocess: clang -E -P -I headers test01.c -o test01.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T01:INCLUDE-H1:BEGIN*/
#include "h1.h"
int __refold_ins__t01_after = 0; /* pure insertion at after boundary for t01_after_marker */
/*BOUNDARY:T01:INCLUDE-H1:END*/

int t01_after_marker = 10;

int main(void) {
  return 0;
}
