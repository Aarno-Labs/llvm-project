// RUN: clang -E -P -I %S/headers --refold-map=%t.json %s -o %t.i
// RUN: FileCheck %s --check-prefix=MAP --implicit-check-not=RF_COMMENTED_OUT \
// RUN:   --implicit-check-not=RF_LINE_COMMENTED_OUT < %t.json

// A `#` inside a comment does not introduce a directive.  Scanning raw lines
// without comment state invents conditional groups out of commented-out
// examples, and a spurious `#if`/`#endif` unbalances the nesting stack, which
// then mis-attributes the extent of the real groups around it.

#define RF_REAL_CONDITION 1
#include "cond_commented_directive.h"
int use = cond_commented_taken;

// The real condition is recorded...
// MAP: "cond": "{{.*}}RF_REAL_CONDITION{{.*}}"

// ...and neither commented-out example became a group.  The absence is checked
// with --implicit-check-not so it covers the whole map: a spurious group is
// emitted *before* the real one, which a trailing MAP-NOT would not reach.
