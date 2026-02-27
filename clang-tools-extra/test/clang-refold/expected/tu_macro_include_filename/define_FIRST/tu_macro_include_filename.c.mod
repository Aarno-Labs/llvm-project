// RUN: %clang-refold-tester-with-lines tu_macro_include_filename FIRST
// RUN: %clang-refold-tester-with-lines tu_macro_include_filename SECOND
// test18.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test18.c -o test18.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define T18_HDR "h1.h"
/*BOUNDARY:T18:MACRO-INCLUDE:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#line 10 "tu_macro_include_filename.c"
#include T18_HDR
/*BOUNDARY:T18:MACRO-INCLUDE:END*/
RF_MARK(t18_after)

int main(void) {
  return 0;
}
