// RUN: %clang-refold-tester-with-lines header_inactive_char_constant_if_line_control_resync
#define LOC 1020 "active_char_header.c"
#if 'A' - 'A'
#define LOC 220 "inactive_char_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 1021 "active_char_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
