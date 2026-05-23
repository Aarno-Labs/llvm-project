// RUN: %clang-refold-tester-with-lines tu_multiline_arg_ownline_obj
// line-layout counterexample: TU multiline macro argument owns __LINE__ site
#define LOC 500 "logical_arg_tu_ownline.c"
#define ID(x) x
#line LOC
int a = 1; int observed = 502;
