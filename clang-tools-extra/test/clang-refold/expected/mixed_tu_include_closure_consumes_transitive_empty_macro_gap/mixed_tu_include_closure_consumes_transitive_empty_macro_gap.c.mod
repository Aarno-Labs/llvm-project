// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_transitive_empty_macro_gap
#define KEEP(x) ((x) + 1)
#define EMPTY
#define WRAP_EMPTY EMPTY

int untouched = KEEP(5);

int arr[] = { 3
WRAP_EMPTY
};
