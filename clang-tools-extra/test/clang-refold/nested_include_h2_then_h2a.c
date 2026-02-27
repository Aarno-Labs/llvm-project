// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a FIRST
// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a SECOND
// RUN: %clang-refold-tester-with-lines nested_include_h2_then_h2a THIRD
// test02.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test02.c -o test02.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T02:INCLUDE-H2:BEGIN*/
#include "h2.h"
/*BOUNDARY:T02:INCLUDE-H2:END*/

RF_MARK(t02_after)

int main(void) {
  return 0;
}
