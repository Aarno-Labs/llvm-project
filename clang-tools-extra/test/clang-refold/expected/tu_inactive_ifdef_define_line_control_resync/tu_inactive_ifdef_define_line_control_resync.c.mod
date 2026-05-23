// RUN: %clang-refold-tester-with-lines tu_inactive_ifdef_define_line_control_resync
#define LOC 940 "active_ifdef_main.c"
#ifdef NEVER_DEFINED_FOR_LINE_CONTROL
#define LOC 140 "inactive_ifdef_main.c"
#endif
#line LOC
int inserted = 0;
#line 940 "active_ifdef_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
