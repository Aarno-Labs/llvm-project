// RUN: %clang-refold-tester-with-lines header_multiline_arg_ownline_func
// line-layout counterexample: header multiline macro argument owns __LINE__ site
#define LOC(n, f) n f
#define ID(x) x
#line LOC(1100, "logical_arg_header_ownline_func.c")
int a = 1; int observed = 1102;
