// RUN: %clang-refold-tester-with-lines tu_inactive_equality_if_line_control_resync
#define FLAG 1
#define LOC 950 "active_eq_main.c"
#if FLAG == 0
#define LOC 150 "inactive_eq_main.c"
#endif
#line LOC
int inserted = 0;
#line 950 "active_eq_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
