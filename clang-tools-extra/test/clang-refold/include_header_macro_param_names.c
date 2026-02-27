// RUN: %clang-refold-tester-with-lines include_header_macro_param_names

// test27.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test27.c -o test27.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#include "h11_param_names.h"
RF_MARK(t27_after)

int main(void) {
  return 0;
}
