// RUN: %clang-refold-tester pragma_once_guard_two_direct_includes
// Regression: a `#pragma once` header inlined into the TU must carry synthetic
// once-state, and the second include of the same physical header must be
// guarded so it does not re-enter a body that is already in the output.
#include "guard_once.h"

int mid = 0;

#include "guard_once.h"

int tail = GUARD_ONCE_V;
