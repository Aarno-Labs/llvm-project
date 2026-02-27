// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FIRST
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 SECOND
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 THIRD
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FOURTH
// RUN: %clang-refold-tester-with-lines tu_if_arm_multiple_includes_h1_and_h6 FIFTH
// test16.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test16.c -o test16.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T16:IF-GROUP:BEGIN*/
#if 1
/*BOUNDARY:T16:IF-ARM:BEGIN*/
#include "h1.h"
#line 1 "headers/h6_defs_undefs.h"
#ifndef RF_H6_DEFS_UNDEFS_H
#define RF_H6_DEFS_UNDEFS_H

#include "common.h"

/*BOUNDARY:H6:BEGIN*/
#define RF_H6_LOCAL(x) x
RF_MARK(h6_after_define)

/* boundary around undef */
#undef RF_H6_LOCAL
int __refold_ins__h6_after_define = 0; /* pure insertion at after boundary for h6_after_define_marker */
#line 12 "headers/h6_defs_undefs.h"
RF_MARK(h6_after_undef)
/*BOUNDARY:H6:END*/

#endif
#line 16 "tu_if_arm_multiple_includes_h1_and_h6.c"
RF_MARK(t16_inside_if_after_includes)
/*BOUNDARY:T16:IF-ARM:END*/
#else
RF_PAYLOAD(t16_else)
#endif
/*BOUNDARY:T16:IF-GROUP:END*/
RF_MARK(t16_after)

int main(void) {
  return 0;
}
