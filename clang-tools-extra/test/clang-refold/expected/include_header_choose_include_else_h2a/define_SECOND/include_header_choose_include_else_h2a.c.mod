// RUN: %clang-refold-tester-with-lines include_header_choose_include_else_h2a FIRST
// RUN: %clang-refold-tester-with-lines include_header_choose_include_else_h2a SECOND
// test21.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test21.c -o test21.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_PICK_H1 0
/*BOUNDARY:T21:INCLUDE-H9:BEGIN*/
#include "h9_choose_include.h"
/*BOUNDARY:T21:INCLUDE-H9:END*/
RF_MARK(t21_after)

int __refold_ins__t21_after = 0; /* pure insertion at after boundary for t21_after_marker */
#line 14 "include_header_choose_include_else_h2a.c"
int main(void) {
  return 0;
}
