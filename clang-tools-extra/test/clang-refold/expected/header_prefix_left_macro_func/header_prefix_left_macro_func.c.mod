// RUN: %clang-refold-tester-with-lines header_prefix_left_macro_func
#define LOC(n, f) n f
#define B_STMT(x) int b = x;
#line LOC(1100, "logical_prefix_left_macro_header_func.c")
int a = 1; B_STMT(2)
#line 1102 "logical_prefix_left_macro_header_func.c"
int observed = __LINE__;
