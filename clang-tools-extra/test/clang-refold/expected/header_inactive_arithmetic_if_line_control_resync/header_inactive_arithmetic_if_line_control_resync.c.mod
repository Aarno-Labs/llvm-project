// RUN: %clang-refold-tester-with-lines header_inactive_arithmetic_if_line_control_resync
#define LOC 940 "active_arith_header.c"
#if 1 - 1
#define LOC 130 "inactive_arith_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 941 "active_arith_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
