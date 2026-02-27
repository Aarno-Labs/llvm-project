#pragma once

// Keep these names unique-ish to discourage earlier token alignment drift.
static inline void ms_call_unique(int x) { (void)x; }
static inline void other_call_unique(int x) { (void)x; }

static inline void ms_outer(void) {
  int ms_unique_arg_123 = 7;

  // This statement is in the PARENT include (depth=1).
  // It ends with the safe-boundary suffix: ")" ";".
  ms_call_unique(ms_unique_arg_123);

  // Include seam INSIDE a function body.
  // The marker declaration below lives in the CHILD include (depth=2).
#include "ms_child2.h"

  other_call_unique(ms_unique_arg_123);
}
