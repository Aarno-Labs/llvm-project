// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a FIRST
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a SECOND
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a THIRD
// RUN: %clang-refold-tester-with-lines include_header_conditional_nested_include_h2a FOURTH
// test30.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test30.c -o test30.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T30:INCLUDE-H13:BEGIN*/
#line 1 "headers/h13_nested_mix.h"
#ifndef RF_H13_NESTED_MIX_H
#define RF_H13_NESTED_MIX_H
#include "common.h"

/*BOUNDARY:H13:BEGIN*/
RF_MARK(h13_begin)
#if 1
  /*BOUNDARY:H13:IF-ARM:INCLUDE-H2A:BEGIN*/
  int __refold_ins__h2a_begin = 0; /* pure insertion at before boundary for h2a_begin_marker */
#line 9 "headers/h13_nested_mix.h"
#include "h2a.h"
  /*BOUNDARY:H13:IF-ARM:INCLUDE-H2A:END*/
#endif
RF_MARK(h13_end)
/*BOUNDARY:H13:END*/

#endif
#line 12 "include_header_conditional_nested_include_h2a.c"
/*BOUNDARY:T30:INCLUDE-H13:END*/
RF_MARK(t30_after)

int main(void) {
  return 0;
}
