// RUN: %clang-refold-tester-with-lines include_header_line_splice_and_comments FIRST
// RUN: %clang-refold-tester-with-lines include_header_line_splice_and_comments SECOND
// RUN: %clang-refold-tester-with-lines include_header_line_splice_and_comments THIRD
// test19.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test19.c -o test19.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T19:INCLUDE-H8:BEGIN*/
#include "h8_splices_comments.h"
/*BOUNDARY:T19:INCLUDE-H8:END*/
RF_MARK(t19_after)

int __refold_ins__t19_after = 0; /* pure insertion at after boundary for t19_after_marker */
int main(void) {
  return 0;
}
