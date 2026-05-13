// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_macro_include_target
#define KEEP(x) ((x) + 1)
#define HDR "two.inc"

int untouched = KEEP(5);

int arr[] = { 3
};
