// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_tu_conditional_macro_directive_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 3
};
