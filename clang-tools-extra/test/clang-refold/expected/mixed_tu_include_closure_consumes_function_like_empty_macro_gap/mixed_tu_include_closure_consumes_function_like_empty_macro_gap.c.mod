// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_function_like_empty_macro_gap
#define KEEP(x) ((x) + 1)
#define EMPTY
#define FORWARD(x) x

int untouched = KEEP(5);

int arr[] = { 3
};
