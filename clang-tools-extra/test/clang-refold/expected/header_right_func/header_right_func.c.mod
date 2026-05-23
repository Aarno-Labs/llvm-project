// RUN: %clang-refold-tester-with-lines header_right_func
#define LOC(n, f) n f
#line LOC(700, "logical_right_func_header.c")
int a = 1; int observed = 701;
