// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_split_include_runs
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 9
};
