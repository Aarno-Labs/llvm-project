// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a FIRST
// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a SECOND
// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a THIRD
// test02.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test02.c -o test02.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T02:INCLUDE-H2:BEGIN*/
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
#line 11 "nested_include_h2_then_h2a.c"
/*BOUNDARY:T02:INCLUDE-H2:END*/

RF_MARK(t02_after)

int main(void) {
  return 0;
}
