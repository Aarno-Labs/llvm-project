// RUN: %clang-refold-tester-with-lines stolen_boundary_multishift_n3_conditional_arm_compound_literal_tail

// test32.c: stolen-boundary-token seam-repair guard (multi-step n=3)
//
// Goal
// ----
// Like test31, but with a longer trailing safe-token run at the end of the
// active arm. We use a compound literal call inside a block so the tail of the
// arm contains the run "} ) ; }" (the first "}" closes the compound literal,
// then ") ;" closes the call, and the final "}" closes the block).
//
// Expected behavior
// -----------------
// In the presence of a "stolen boundary token" alignment, the seam-repair can
// shift three tokens (n=3) as long as it stays within a stable structural
// context (same include / same conditional arm). It must not drift across the
// conditional boundary.
//
// How to use
// ----------
//  1) Produce A: clang -E -P -I headers test32.c -o test32.c.i (or your
//     instrumented Clang that also emits test32.c.refold.json).
//  2) Produce B by inserting this line into the PP output between the closing
//     brace of the active arm and the t32_after_marker line:
//
//       sink_ptr((int[]){1});
//
//     (see test32.c.i.mod.1).
//  3) Run clang-refold and compare against test32.c.mod.1.

#include "common.h"

static void sink_ptr(const int *p) { (void)p; }

#if 1
  // Active arm ends with the safe-token run "} ) ; }".
  { sink_ptr((int[]){1}); }
#else
  { sink_ptr((int[]){2}); }
#endif
sink_ptr((int[]){1});
#line 41 "stolen_boundary_multishift_n3_conditional_arm_compound_literal_tail.c"

RF_MARK(t32_after)

int main(void) {
  return 0;
}
