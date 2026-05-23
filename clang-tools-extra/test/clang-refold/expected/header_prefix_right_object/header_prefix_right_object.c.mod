// RUN: %clang-refold-tester-with-lines header_prefix_right_object
// header prefix-collapse plus observer-right-merge object-like case
#define LOC 800 "logical_prefix_right_header.c"
#line LOC
int a = 1; int b = 2;
#line 802 "logical_prefix_right_header.c"
int observed = __LINE__;
