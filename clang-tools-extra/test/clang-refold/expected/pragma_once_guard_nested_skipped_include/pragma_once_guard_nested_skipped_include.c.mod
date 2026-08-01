// RUN: %clang-refold-tester pragma_once_guard_nested_skipped_include
// Regression: the second include inside the parent header is suppressed by
// `#pragma once`, so the producer records no parent for it and it is invisible
// to the include child index.  It must still be guarded when the parent is
// materialized, or the refolded TU re-enters an already inlined body.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#define GUARD_ONCE_V 1
int from_guard_once = 2;
#endif
int parent_mid = 1;
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once.h"
#endif

int tail = GUARD_ONCE_V;
