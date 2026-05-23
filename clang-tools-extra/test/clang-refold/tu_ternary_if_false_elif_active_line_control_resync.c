// RUN: %clang-refold-tester-with-lines tu_ternary_if_false_elif_active_line_control_resync
#define LOC 990 "base_elif_main.c"
#if 1 ? 0 : 1
#define LOC 190 "inactive_if_arm_main.c"
#elif 1
#define LOC 990 "active_elif_main.c"
#endif
#line LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
