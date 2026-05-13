// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_empty_macro_gap
#define KEEP(x) ((x) + 1)
#define EMPTY

int untouched = KEEP(5);

int arr[] = { 3
};
