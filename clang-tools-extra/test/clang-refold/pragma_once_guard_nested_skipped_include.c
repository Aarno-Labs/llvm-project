// RUN: %clang-refold-tester pragma_once_guard_nested_skipped_include
// Regression: the second include inside the parent header is suppressed by
// `#pragma once`, so the producer records no parent for it and it is invisible
// to the include child index.  It must still be guarded when the parent is
// materialized, or the refolded TU re-enters an already inlined body.
#include "guard_once_parent.h"

int tail = GUARD_ONCE_V;
