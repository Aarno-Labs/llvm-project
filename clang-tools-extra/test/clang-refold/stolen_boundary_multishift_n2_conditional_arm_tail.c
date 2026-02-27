// RUN: %clang-refold-tester-with-lines stolen_boundary_multishift_n2_conditional_arm_tail

// test31.c: stolen-boundary-token seam-repair guard (multi-step n=2)
//
// Goal
// ----
// Exercise the multi-step stolen-boundary-token seam-repair on a pure insertion
// that sits at a conditional-group boundary, where the A-stream contains a
// trailing delimiter run of safe tokens at the end of the active arm.
//
// Expected behavior
// -----------------
// When the A->B diff "steals" one or more of the trailing delimiter tokens from
// the end of the active arm (here the run is ") ; }"), the seam-repair should
// be able to shift at least two tokens (n=2) as long as the structural context
// remains stable, but must not drift across conditional-arm boundaries.
//
// How to use
// ----------
//  1) Produce A: clang -E -P -I headers test31.c -o test31.c.i (or your
//     instrumented Clang that also emits test31.c.refold.json).
//  2) Produce B by inserting this line into the PP output between the closing
//     brace of the active arm and the t31_after_marker line:
//
//       sink_int(0);
//
//     (see test31.c.i.mod.1).
//  3) Run clang-refold and compare against test31.c.mod.1.

#include "common.h"

static void sink_int(int x) { (void)x; }

#if 1
  // Active arm: ends with the safe-token run ") ; }" (from "sink_int(0); }").
  { sink_int(0); }
#else
  { sink_int(1); }
#endif

RF_MARK(t31_after)

int main(void) {
  return 0;
}
