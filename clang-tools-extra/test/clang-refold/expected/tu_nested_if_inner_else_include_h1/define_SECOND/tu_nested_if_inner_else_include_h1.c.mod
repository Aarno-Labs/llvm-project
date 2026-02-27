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
#include "h1.h"
/*BOUNDARY:T26:INNER-ELSE:INCLUDE-H1:END*/
#endif
/*BOUNDARY:T26:INNER-IF:END*/
#endif
/*BOUNDARY:T26:OUTER-IF:END*/
RF_MARK(t26_after)

int __refold_ins__t26_after = 0; /* pure insertion at after boundary for t26_after_marker */
#line 23 "tu_nested_if_inner_else_include_h1.c"
int main(void) {
  return 0;
}
