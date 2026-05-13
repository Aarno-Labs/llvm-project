// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_nested_empty_include_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 3
};
