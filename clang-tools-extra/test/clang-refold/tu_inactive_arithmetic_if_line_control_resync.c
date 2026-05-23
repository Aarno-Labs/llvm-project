// RUN: %clang-refold-tester-with-lines tu_inactive_arithmetic_if_line_control_resync
#define LOC 930 "active_arith_main.c"
#if 1 - 1
#define LOC 120 "inactive_arith_main.c"
#endif
#line LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
