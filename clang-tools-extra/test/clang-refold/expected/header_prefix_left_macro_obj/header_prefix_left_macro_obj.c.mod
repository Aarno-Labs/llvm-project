// RUN: %clang-refold-tester-with-lines header_prefix_left_macro_obj
#define LOC 900 "logical_prefix_left_macro_header.c"
#define B_STMT int b = 2;
#line LOC
int a = 1; B_STMT
#line 902 "logical_prefix_left_macro_header.c"
int observed = __LINE__;
