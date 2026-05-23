// RUN: %clang-refold-tester-with-lines tu_inactive_if0_define_line_control_resync
#define LOC 930 "active_if0_main.c"
#if 0
#define LOC 120 "inactive_if0_main.c"
#endif
#line LOC
int inserted = 0;
#line 930 "active_if0_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
