// RUN: %clang-refold-tester-with-lines header_right_macro_func
// line observer right-extension: header function-like macro-produced suffix token
#define LOC(n, f) n f
#define SEMI ;
#line LOC(1100, "logical_right_macro_header_func.c")
int a = 1; int observed = 1101 SEMI
