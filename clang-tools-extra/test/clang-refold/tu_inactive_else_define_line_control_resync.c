// RUN: %clang-refold-tester-with-lines tu_inactive_else_define_line_control_resync
#define CHOOSE_ACTIVE_LOC 1
#if CHOOSE_ACTIVE_LOC
#define LOC 970 "active_else_main.c"
#else
#define LOC 170 "inactive_else_main.c"
#endif
#line LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
