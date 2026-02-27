// RUN: %clang-refold-tester-with-lines tu_if0_else_include_h2 FIRST
// RUN: %clang-refold-tester-with-lines tu_if0_else_include_h2 SECOND
// RUN: %clang-refold-tester-with-lines tu_if0_else_include_h2 THIRD
// test11.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test11.c -o test11.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T11:IF-GROUP:BEGIN*/
#if 0
  /*BOUNDARY:T11:IF-ARM:BEGIN*/
  RF_PAYLOAD(t11_if)
  /*BOUNDARY:T11:IF-ARM:END*/
#else
/*BOUNDARY:T11:ELSE-ARM:INCLUDE-H2:BEGIN*/
#line 1 "headers/h2.h"
#ifndef RF_H2_H
#define RF_H2_H

#include "common.h"
/*BOUNDARY:H2:BEGIN*/
RF_MARK(h2_begin)
int __refold_ins__h2a_begin = 0; /* pure insertion at before boundary for h2a_begin_marker */
#line 7 "headers/h2.h"
#include "h2a.h"
RF_PAYLOAD(h2)
/*BOUNDARY:H2:END*/

#endif
#line 17 "tu_if0_else_include_h2.c"
/*BOUNDARY:T11:ELSE-ARM:INCLUDE-H2:END*/
#endif
/*BOUNDARY:T11:IF-GROUP:END*/
RF_MARK(t11_after)

int main(void) {
  return 0;
}
