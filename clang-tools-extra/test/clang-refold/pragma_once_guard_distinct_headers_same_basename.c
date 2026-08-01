// RUN: %clang-refold-tester pragma_once_guard_distinct_headers_same_basename
// Regression: two different physical headers sharing a basename must receive
// distinct guard macros.  Keying by basename would collapse them and suppress
// one of the two bodies.
#include "guard_once.h"
#include "sub/guard_once.h"

int mid = 0;

#include "guard_once.h"
#include "sub/guard_once.h"

int tail = GUARD_ONCE_V + SUB_GUARD_V;
