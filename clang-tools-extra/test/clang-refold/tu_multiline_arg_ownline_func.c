// RUN: %clang-refold-tester-with-lines tu_multiline_arg_ownline_func
// line-layout counterexample: TU multiline macro argument owns __LINE__ site
#define LOC(n, f) n f
#define ID(x) x
#line LOC(700, "logical_arg_tu_ownline_func.c")
int a = 1;
int observed = ID(
    __LINE__
);
