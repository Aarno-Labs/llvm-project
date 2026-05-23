// RUN: %clang-refold-tester-with-lines command_line_line_loc
#ifndef LOC
#define LOC 900 "logical_cmdline.c"
#endif
#line LOC
int observed = __LINE__;
