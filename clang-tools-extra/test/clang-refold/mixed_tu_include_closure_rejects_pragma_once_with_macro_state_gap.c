// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_pragma_once_with_macro_state_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "pragma_once_define_gap.inc"
#include "two.inc"
};
