// RUN: %clang-refold-tester-with-lines mixed_owner_closure_ignores_preserved_define_builtin
// Step 1 counterexample: the edited PP hunk consumes a TU-spelled macro
// invocation whose only material token is a predefined builtin, plus a
// top-level include expansion.  The source closure has macro + include owners
// but no ordinary TU-owned PP token in the consumed A range.
#define MAKE_LEFT() __COUNTER__

int values[] = {
  MAKE_LEFT()
#include "headers/right_tail.h"
};
