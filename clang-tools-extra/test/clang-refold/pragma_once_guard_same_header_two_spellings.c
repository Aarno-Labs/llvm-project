// RUN: %clang-refold-tester pragma_once_guard_same_header_two_spellings
// Regression: guards are keyed by physical header identity, not by the include
// spelling.  Both directives open the same file, so they must resolve to one
// guard macro and the second occurrence must be wrapped with it.
#include "guard_once.h"

int mid = 0;

#include "./guard_once.h"

int tail = GUARD_ONCE_V;
