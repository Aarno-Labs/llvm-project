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
#line 14 "tu_sequential_includes_h1_then_h2.c"
/*BOUNDARY:T10:SEQ-INCLUDES:END*/
RF_MARK(t10_after)

int main(void) {
  return 0;
}
