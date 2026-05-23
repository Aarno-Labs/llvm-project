// RUN: %clang-refold-tester-with-lines prefix_left_macro_obj
#define LOC 500 "logical_prefix_left_macro_tu.c"
#define B_STMT int b = 2;
#line LOC
int a = 1; B_STMT
#line 502 "logical_prefix_left_macro_tu.c"
int observed = __LINE__;
