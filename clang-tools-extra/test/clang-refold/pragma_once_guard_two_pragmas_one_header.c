// RUN: %clang-refold-tester pragma_once_guard_two_pragmas_one_header
// Regression: every `#pragma once` in a header becomes a `#define` of the same
// guard macro.  The repeated define is a benign redefinition because both are
// object-like with an empty replacement list.
#include "guard_once_twice.h"

int mid = 0;

#include "guard_once_twice.h"

int tail = GUARD_TWICE_V;
