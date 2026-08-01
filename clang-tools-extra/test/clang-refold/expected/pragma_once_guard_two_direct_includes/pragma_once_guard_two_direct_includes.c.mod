// RUN: %clang-refold-tester pragma_once_guard_two_direct_includes
// Regression: a `#pragma once` header inlined into the TU must carry synthetic
// once-state, and the second include of the same physical header must be
// guarded so it does not re-enter a body that is already in the output.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#define GUARD_ONCE_V 1
int from_guard_once = 2;
#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once.h"
#endif

int tail = GUARD_ONCE_V;
