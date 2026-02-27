// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled FIRST
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled SECOND
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled THIRD
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_enabled FOURTH
// test06.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test06.c -o test06.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_ENABLE_EXTRA 1
/*BOUNDARY:T06:INCLUDE-H4:BEGIN*/
#line 1 "headers/h4_conditional_include.h"
#ifndef RF_H4_COND_INC_H
#define RF_H4_COND_INC_H

#include "common.h"

/*BOUNDARY:H4:BEGIN*/
RF_MARK(h4_begin)

#if RF_ENABLE_EXTRA
  /*BOUNDARY:H4:IF-ARM:INCLUDE-H1:BEGIN*/
  int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#line 11 "headers/h4_conditional_include.h"
#include "h1.h"
  /*BOUNDARY:H4:IF-ARM:INCLUDE-H1:END*/
#else
  /*BOUNDARY:H4:ELSE-ARM:PAYLOAD:BEGIN*/
  RF_PAYLOAD(h4_else_payload)
  /*BOUNDARY:H4:ELSE-ARM:PAYLOAD:END*/
#endif

RF_MARK(h4_end)
/*BOUNDARY:H4:END*/

#endif
#line 13 "include_h4_conditional_include_enabled.c"
/*BOUNDARY:T06:INCLUDE-H4:END*/
RF_MARK(t06_after)

int main(void) {
  return 0;
}
