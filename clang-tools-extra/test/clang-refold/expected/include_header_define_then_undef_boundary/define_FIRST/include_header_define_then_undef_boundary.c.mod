// RUN: %clang-refold-tester-with-lines include_header_define_then_undef_boundary FIRST
// RUN: %clang-refold-tester-with-lines include_header_define_then_undef_boundary SECOND
// RUN: %clang-refold-tester-with-lines include_header_define_then_undef_boundary THIRD
// test13.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test13.c -o test13.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T13:INCLUDE-H6:BEGIN*/
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
#line 11 "include_header_define_then_undef_boundary.c"
/*BOUNDARY:T13:INCLUDE-H6:END*/
RF_MARK(t13_after)

int main(void) {
  return 0;
}
