// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_undef_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "toggle.inc"
#include "two.inc"
};
