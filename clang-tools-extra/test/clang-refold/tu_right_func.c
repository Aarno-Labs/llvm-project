// RUN: %clang-refold-tester-with-lines tu_right_func
#define LOC(n, f) n f
#line LOC(700, "logical_right_func_tu.c")
int a = 1;
int observed = __LINE__
;
