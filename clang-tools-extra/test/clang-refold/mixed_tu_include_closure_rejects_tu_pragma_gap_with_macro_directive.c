// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive
// Fail-closed companion to the B-payload/preserved-source partition: the owner
// gap holds a second directive beside the pragma.  Only the pragma is submitted
// as a preserved piece, so the shared source-gap theorem reports the `#define`
// as an unowned protected interval and the closure is refused.  A partition
// that emitted only the pragma would silently drop the macro-state transition.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#define GAP_VALUE 99
#include "two_seven.inc"
};
