// RUN: %clang-refold-tester-with-lines tu_nested_if_inner_else_include_h1 FIRST
// RUN: %clang-refold-tester-with-lines tu_nested_if_inner_else_include_h1 SECOND
// test26.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test26.c -o test26.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T26:OUTER-IF:BEGIN*/
#if 1
/*BOUNDARY:T26:INNER-IF:BEGIN*/
#if 0
    /* empty */
#else
/*BOUNDARY:T26:INNER-ELSE:INCLUDE-H1:BEGIN*/
int __refold_ins__h1_begin = 0; /* pure insertion at before boundary for h1_begin_marker */
#include "h1.h"
/*BOUNDARY:T26:INNER-ELSE:INCLUDE-H1:END*/
#endif
/*BOUNDARY:T26:INNER-IF:END*/
#endif
/*BOUNDARY:T26:OUTER-IF:END*/
#line 21 "tu_nested_if_inner_else_include_h1.c"
RF_MARK(t26_after)

int main(void) {
  return 0;
}
