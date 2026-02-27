// RUN: %clang-refold-tester-with-lines include_header_choose_include_if_h1 FIRST
// RUN: %clang-refold-tester-with-lines include_header_choose_include_if_h1 SECOND
// test20.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test20.c -o test20.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define RF_PICK_H1 1
/*BOUNDARY:T20:INCLUDE-H9:BEGIN*/
#include "h9_choose_include.h"
/*BOUNDARY:T20:INCLUDE-H9:END*/
RF_MARK(t20_after)

int main(void) {
  return 0;
}
