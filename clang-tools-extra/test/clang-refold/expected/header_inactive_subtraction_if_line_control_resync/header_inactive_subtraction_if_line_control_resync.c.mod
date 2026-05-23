// RUN: %clang-refold-tester-with-lines header_inactive_subtraction_if_line_control_resync
#define LOC 980 "active_sub_header.c"
#if 3 - 3
#define LOC 180 "inactive_sub_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 981 "active_sub_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
