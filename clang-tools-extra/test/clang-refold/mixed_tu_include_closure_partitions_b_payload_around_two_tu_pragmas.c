// RUN: %clang-refold-tester mixed_tu_include_closure_partitions_b_payload_around_two_tu_pragmas
// Two preserved directives share one owner gap.  Each is projected to its own A
// frontier and emitted at its own derived split, in source order, so their
// relative order and their position against the realized B payload are both
// preserved.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#pragma GCC poison BAR
#include "two_seven.inc"
};
