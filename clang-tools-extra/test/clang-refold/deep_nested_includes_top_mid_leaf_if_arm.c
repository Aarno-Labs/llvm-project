// RUN: %clang-refold-tester-with-lines deep_nested_includes_top_mid_leaf_if_arm

// test24.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test24.c -o test24.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

/*BOUNDARY:T24:INCLUDE-H10TOP:BEGIN*/
#include "h10_top.h"
/*BOUNDARY:T24:INCLUDE-H10TOP:END*/
RF_MARK(t24_after)

int main(void) {
  return 0;
}
