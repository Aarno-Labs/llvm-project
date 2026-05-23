// RUN: %clang-refold-tester-with-lines header_ternary_if_false_elif_active_line_control_resync
#line 1 "headers/h_ternary_elif.h"
#define LOC 995 "base_elif_header.c"
#if 1 ? 0 : 1
#define LOC 195 "inactive_if_arm_header.c"
#elif 1
#define LOC 995 "active_elif_header.c"
#endif
#line LOC
int head = __LINE__;
int inserted = 0;
#line 996 "active_elif_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_ternary_if_false_elif_active_line_control_resync.c"
int tail = 3;
