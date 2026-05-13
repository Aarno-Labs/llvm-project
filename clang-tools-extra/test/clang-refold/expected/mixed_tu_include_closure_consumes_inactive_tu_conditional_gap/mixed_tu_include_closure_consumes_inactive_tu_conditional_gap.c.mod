// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_inactive_tu_conditional_gap
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 3
#if 0
#define DEAD_VALUE 99
int dead = KEEP(10);
#endif
};
