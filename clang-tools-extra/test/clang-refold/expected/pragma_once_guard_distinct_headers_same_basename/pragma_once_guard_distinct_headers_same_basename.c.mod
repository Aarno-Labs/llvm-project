// RUN: %clang-refold-tester pragma_once_guard_distinct_headers_same_basename
// Regression: two different physical headers sharing a basename must receive
// distinct guard macros.  Keying by basename would collapse them and suppress
// one of the two bodies.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#define GUARD_ONCE_V 1
int from_guard_once = 2;
#endif
#ifndef __CLANG_REFOLD_ONCE_2
#define __CLANG_REFOLD_ONCE_2
#define SUB_GUARD_V 6
int from_sub_guard = 7;
#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once.h"
#endif
#ifndef __CLANG_REFOLD_ONCE_2
#define __CLANG_REFOLD_ONCE_2
#include "sub/guard_once.h"
#endif

int tail = GUARD_ONCE_V + SUB_GUARD_V;
