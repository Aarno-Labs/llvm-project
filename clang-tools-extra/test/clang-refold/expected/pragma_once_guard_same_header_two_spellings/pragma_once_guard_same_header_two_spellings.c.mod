// RUN: %clang-refold-tester pragma_once_guard_same_header_two_spellings
// Regression: guards are keyed by physical header identity, not by the include
// spelling.  Both directives open the same file, so they must resolve to one
// guard macro and the second occurrence must be wrapped with it.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#define GUARD_ONCE_V 1
int from_guard_once = 2;
#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "./guard_once.h"
#endif

int tail = GUARD_ONCE_V;
