// RUN: %clang-refold-tester-with-lines header_right_macro_obj
// line observer right-extension: header object-like macro-produced suffix token
#define LOC 900 "logical_right_macro_header.c"
#define SEMI ;
#line LOC
int a = 1; int observed = 901 SEMI
