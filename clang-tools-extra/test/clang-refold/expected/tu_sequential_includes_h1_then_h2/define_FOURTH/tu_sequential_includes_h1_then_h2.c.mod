// RUN: %clang-refold-tester-with-lines tu_sequential_includes_h1_then_h2 FIRST
// RUN: %clang-refold-tester-with-lines tu_sequential_includes_h1_then_h2 SECOND
// RUN: %clang-refold-tester-with-lines tu_sequential_includes_h1_then_h2 THIRD
// RUN: %clang-refold-tester-with-lines tu_sequential_includes_h1_then_h2 FOURTH
// test10.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test10.c -o test10.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T10:SEQ-INCLUDES:BEGIN*/
#include "h1.h"
/*BOUNDARY:T10:BETWEEN-INCLUDES*/
#include "h2.h"
/*BOUNDARY:T10:SEQ-INCLUDES:END*/
RF_MARK(t10_after)

int __refold_ins__t10_after = 0; /* pure insertion at after boundary for t10_after_marker */
#line 17 "tu_sequential_includes_h1_then_h2.c"
int main(void) {
  return 0;
}
