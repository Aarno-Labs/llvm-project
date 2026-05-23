// RUN: %clang-refold-tester-with-lines tu_prefix_right_object
// tu prefix-collapse plus observer-right-merge object-like case
#define LOC 500 "logical_prefix_right_tu.c"
#line LOC
int a = 1; int b = 2;
#line 502 "logical_prefix_right_tu.c"
int observed = __LINE__;
