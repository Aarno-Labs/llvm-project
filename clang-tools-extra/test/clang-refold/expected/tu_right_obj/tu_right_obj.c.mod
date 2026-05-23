// RUN: %clang-refold-tester-with-lines tu_right_obj
#define LOC 500 "logical_right_tu.c"
#line LOC
int a = 1; int observed = 501;
