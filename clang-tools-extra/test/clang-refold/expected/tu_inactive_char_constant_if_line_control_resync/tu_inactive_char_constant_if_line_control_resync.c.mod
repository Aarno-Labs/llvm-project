// RUN: %clang-refold-tester-with-lines tu_inactive_char_constant_if_line_control_resync
#define LOC 1010 "active_char_main.c"
#if 'A' - 'A'
#define LOC 210 "inactive_char_main.c"
#endif
#line LOC
int inserted = 0;
#line 1010 "active_char_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
