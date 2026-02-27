// RUN: %clang-refold-tester-with-lines include_pragma_once_header_with_conditional FIRST
// RUN: %clang-refold-tester-with-lines include_pragma_once_header_with_conditional SECOND
// RUN: %clang-refold-tester-with-lines include_pragma_once_header_with_conditional THIRD
// test12.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test12.c -o test12.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T12:INCLUDE-H5:BEGIN*/
#include "h5_pragma_once.h"
int __refold_ins__h5_end = 0; /* pure insertion at after boundary for h5_end_marker */
#line 11 "include_pragma_once_header_with_conditional.c"
/*BOUNDARY:T12:INCLUDE-H5:END*/
RF_MARK(t12_after)

int main(void) {
  return 0;
}
