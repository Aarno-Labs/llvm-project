// RUN: %clang-refold-tester-with-lines tu_prefix_right_function
// tu prefix-collapse plus observer-right-merge function-like case
#define LOC(n, s) n s
#line LOC(700, "logical_prefix_right_func.c")
int a = 1;
int b = 2;
int observed = __LINE__
;
