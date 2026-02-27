// RUN: %clang-refold-tester-with-lines header_defines_flag_enables_ifdef_include_h1 FIRST
// RUN: %clang-refold-tester-with-lines header_defines_flag_enables_ifdef_include_h1 SECOND
// test29.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test29.c -o test29.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#include "h12_define_flag.h"
/*BOUNDARY:T29:IFDEF:BEGIN*/
#ifdef RF_FLAG_FROM_HEADER
/*BOUNDARY:T29:IFDEF:INCLUDE-H1:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */

#line 12 "header_defines_flag_enables_ifdef_include_h1.c"
#include "h1.h"
/*BOUNDARY:T29:IFDEF:INCLUDE-H1:END*/
#endif
/*BOUNDARY:T29:IFDEF:END*/
RF_MARK(t29_after)

int main(void) {
  return 0;
}
