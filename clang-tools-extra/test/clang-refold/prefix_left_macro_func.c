// RUN: %clang-refold-tester-with-lines prefix_left_macro_func
#define LOC(n, f) n f
#define B_STMT(x) int b = x;
#line LOC(700, "logical_prefix_left_macro_func_tu.c")
int a = 1;
B_STMT(2)
int observed = __LINE__;
