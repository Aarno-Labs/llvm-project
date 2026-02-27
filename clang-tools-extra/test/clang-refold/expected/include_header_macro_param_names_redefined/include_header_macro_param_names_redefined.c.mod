// RUN: %clang-refold-tester-with-lines include_header_macro_param_names_redefined

// test28.c: boundary insertion stress
// Preprocess: clang -E -P -I headers test28.c -o test28.c.i
// Then make pure insertions around markers /*BOUNDARY:...*/ in the PP output.
#include "common.h"

#define h11_p0 alt0
#define h11_p1 alt1
#include "h11_param_names.h"
RF_MARK(t28_after)

int __refold_ins__t28_after = 0; /* pure insertion at after boundary for t28_after_marker */
#line 13 "include_header_macro_param_names_redefined.c"
int main(void) {
  return 0;
}
