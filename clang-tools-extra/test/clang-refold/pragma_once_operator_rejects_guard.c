// RUN: %clang-refold-tester pragma_once_operator_rejects_guard
// Regression: `_Pragma("once")` has real once semantics but the producer records
// no pragma item for it, so no inventory can account for it.  The guard catalog
// must fail closed rather than silently inline the body unguarded.
#include "guard_once_operator.h"

int mid = 0;

#include "guard_once_operator.h"

int tail = GUARD_OP_V;
