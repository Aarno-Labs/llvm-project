// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a FIRST
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a SECOND
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a THIRD
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a FOURTH
// test30.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test30.c -o test30.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T30:INCLUDE-H13:BEGIN*/
#include "h13_nested_mix.h"
/*BOUNDARY:T30:INCLUDE-H13:END*/
RF_MARK(t30_after)

int __refold_ins__t30_after = 0; /* pure insertion at after boundary for t30_after_marker */
int main(void) {
  return 0;
}
