// RUN: %clang-refold-tester-with-lines header_multiline_arg_ownline_obj
// line-layout counterexample: header multiline macro argument owns __LINE__ site
#define LOC 900 "logical_arg_header_ownline.c"
#define ID(x) x
#line LOC
int a = 1; int observed = 902;
