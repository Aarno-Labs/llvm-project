// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_tu_pragma_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two.inc"
};
