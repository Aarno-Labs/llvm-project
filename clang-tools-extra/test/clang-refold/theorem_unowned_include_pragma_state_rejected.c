// RUN: %clang-refold-tester theorem_unowned_include_pragma_state_rejected
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two.inc"
};
