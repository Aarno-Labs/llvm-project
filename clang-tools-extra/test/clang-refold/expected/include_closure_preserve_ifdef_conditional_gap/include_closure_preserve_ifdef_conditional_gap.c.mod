// RUN: %clang-refold-tester include_closure_preserve_ifdef_conditional_gap
#define KEEP(x) ((x) + 1)
#define ENABLE_GAP 1

int untouched = KEEP(5);

int x[] = {
4
#ifdef ENABLE_GAP
#endif
};
