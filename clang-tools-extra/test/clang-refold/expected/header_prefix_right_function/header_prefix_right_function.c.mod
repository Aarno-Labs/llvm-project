// RUN: %clang-refold-tester-with-lines header_prefix_right_function
// header prefix-collapse plus observer-right-merge function-like case
#define LOC(n, s) n s
#line LOC(900, "logical_prefix_right_header_func.c")
int a = 1; int b = 2;
#line 902 "logical_prefix_right_header_func.c"
int observed = __LINE__;
