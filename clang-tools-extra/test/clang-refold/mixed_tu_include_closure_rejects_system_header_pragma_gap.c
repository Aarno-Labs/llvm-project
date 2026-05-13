// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_system_header_pragma_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "system_header_gap.inc"
#include "two.inc"
};
