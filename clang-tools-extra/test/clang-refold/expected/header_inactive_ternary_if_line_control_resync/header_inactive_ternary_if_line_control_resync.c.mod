// RUN: %clang-refold-tester-with-lines header_inactive_ternary_if_line_control_resync
#define LOC 980 "active_ternary_header.c"
#if 1 ? 0 : 1
#define LOC 180 "inactive_ternary_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 981 "active_ternary_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
