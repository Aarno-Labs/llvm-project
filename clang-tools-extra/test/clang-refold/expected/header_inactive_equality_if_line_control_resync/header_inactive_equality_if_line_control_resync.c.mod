// RUN: %clang-refold-tester-with-lines header_inactive_equality_if_line_control_resync
#define FLAG 1
#define LOC 960 "active_eq_header.c"
#if FLAG == 0
#define LOC 160 "inactive_eq_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 961 "active_eq_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
