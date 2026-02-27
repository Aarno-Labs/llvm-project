// RUN: %clang-refold-tester-with-lines tu_if_arm_define_undef_directives

// test23.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test23.c -o test23.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T23:IF-GROUP:BEGIN*/
#if 1
/*BOUNDARY:T23:IF-ARM:DIRS:BEGIN*/
#define T23_X 1
#undef T23_X
/*BOUNDARY:T23:IF-ARM:DIRS:END*/
#else
#include "h1.h"
#endif
/*BOUNDARY:T23:IF-GROUP:END*/
RF_MARK(t23_after)

int main(void) {
  return 0;
}
