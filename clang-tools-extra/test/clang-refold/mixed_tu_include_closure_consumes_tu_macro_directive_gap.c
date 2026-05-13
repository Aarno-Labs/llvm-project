// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_tu_macro_directive_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#define GAP_VALUE 99
#include "two.inc"
};
