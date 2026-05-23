// RUN: %clang-refold-tester-with-lines tu_gnu_named_variadic_line_control_resync
#define LOC(args...) args
#line LOC(1200 "gnu_named_tu.c")
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
