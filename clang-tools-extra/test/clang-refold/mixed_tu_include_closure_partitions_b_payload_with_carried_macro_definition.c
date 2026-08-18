// RUN: %clang-refold-tester mixed_tu_include_closure_partitions_b_payload_with_carried_macro_definition
// The partition must compose with the repairs that already run over the same
// closure.  Here the consumed include also defines a macro that a later
// surviving use observes, so macro-state repair carries that `#define` past the
// replacement while the pragma is preserved inside it.  Definition still
// precedes use, and the pragma still precedes the payload it preceded in A.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two_seven_defines_later.inc"
};

int later = LATER_VALUE;
