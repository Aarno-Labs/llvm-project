// RUN: %clang-refold-tester-with-lines tu_inactive_ifndef_define_line_control_resync
#define ENABLE_LINE_CONTROL_GUARD
#define LOC 980 "active_ifndef_main.c"
#ifndef ENABLE_LINE_CONTROL_GUARD
#define LOC 180 "inactive_ifndef_main.c"
#endif
#line LOC
int inserted = 0;
#line 980 "active_ifndef_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
