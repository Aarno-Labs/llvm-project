// RUN: %clang-refold-tester pragma_once_guard_two_pragmas_one_header
// Regression: every `#pragma once` in a header becomes a `#define` of the same
// guard macro.  The repeated define is a benign redefinition because both are
// object-like with an empty replacement list.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#define GUARD_TWICE_V 3
int from_guard_twice = 7;
#define __CLANG_REFOLD_ONCE_1
#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once_twice.h"
#endif

int tail = GUARD_TWICE_V;
