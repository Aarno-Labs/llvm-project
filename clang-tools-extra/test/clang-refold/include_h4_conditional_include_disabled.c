// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_disabled FIRST
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_disabled SECOND
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_disabled THIRD
// RUN: %clang-refold-tester-with-lines include_h4_conditional_include_disabled TIEBREAK
// Preprocess: clang -E -P -I headers test07.c -o test07.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_ENABLE_EXTRA 0
/*BOUNDARY:T07:INCLUDE-H4:BEGIN*/
#include "h4_conditional_include.h"
/*BOUNDARY:T07:INCLUDE-H4:END*/
RF_MARK(t07_after)

int main(void) {
  return 0;
}
