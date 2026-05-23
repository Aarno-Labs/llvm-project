// RUN: %clang-refold-tester-with-lines right_macro_tu_obj
// line observer right-extension: TU object-like macro-produced suffix token
#define LOC 500 "logical_right_macro_tu.c"
#define SEMI ;
#line LOC
int a = 1; int observed = 501 SEMI
