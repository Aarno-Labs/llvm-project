// RUN: %clang-refold-tester-with-lines right_macro_tu_func
// line observer right-extension: TU function-like macro-produced suffix token
#define LOC(n, f) n f
#define SEMI ;
#line LOC(700, "logical_right_macro_tu_func.c")
int a = 1;
int observed = __LINE__
SEMI
