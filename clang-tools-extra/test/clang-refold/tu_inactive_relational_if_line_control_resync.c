// RUN: %clang-refold-tester-with-lines tu_inactive_relational_if_line_control_resync
#define LOC 970 "active_rel_main.c"
#if 2 < 1
#define LOC 170 "inactive_rel_main.c"
#endif
#line LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
