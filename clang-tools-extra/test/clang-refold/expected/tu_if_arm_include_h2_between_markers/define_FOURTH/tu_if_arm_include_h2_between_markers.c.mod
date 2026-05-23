// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers FIRST
// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers SECOND
// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers THIRD
// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers FOURTH
// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers FIFTH
// RUN: %clang-refold-tester-with-lines tu_if_arm_include_h2_between_markers SIXTH
// test22.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test22.c -o test22.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#include "h1.h"
/*BOUNDARY:T22:IF-GROUP:BEGIN*/
#if 1
/*BOUNDARY:T22:IF-ARM:BEGIN*/
RF_MARK(t22_if_before_include)
#line 1 "headers/h2.h"
#ifndef RF_H2_H
#define RF_H2_H

#include "common.h"
/*BOUNDARY:H2:BEGIN*/
RF_MARK(h2_begin)
int __refold_ins__h2a_begin = 0; /* pure insertion at before boundary for h2a_begin_marker */
#include "h2a.h"
RF_PAYLOAD(h2)
/*BOUNDARY:H2:END*/

#endif
#line 18 "tu_if_arm_include_h2_between_markers.c"
RF_MARK(t22_if_after_include)
/*BOUNDARY:T22:IF-ARM:END*/
#endif
/*BOUNDARY:T22:IF-GROUP:END*/
#line 22 "tu_if_arm_include_h2_between_markers.c"
RF_MARK(t22_after)

int main(void) {
  return 0;
}
