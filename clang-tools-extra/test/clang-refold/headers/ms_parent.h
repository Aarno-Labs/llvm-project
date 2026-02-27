#pragma once

// Keep semicolons scarce and concentrate the boundary right before the include.
// We intentionally create a tail ")) ;" sequence via macro expansion.

#define ms_unique_arg_123 123
#define ms_call_unique(x) ((void)(x))

static inline void ms_trigger(void) {
  // This statement expands to: ((void)(123));
  // The seam immediately after it is where the insertion is intended.
  ms_call_unique(ms_unique_arg_123);

  // Include *inside* the function body so the next tokens in the PP stream come
  // directly from the child snippet.
#include "ms_child.h"
}
