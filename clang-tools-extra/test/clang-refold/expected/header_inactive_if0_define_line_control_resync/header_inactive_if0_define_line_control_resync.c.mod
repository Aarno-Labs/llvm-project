// RUN: %clang-refold-tester-with-lines header_inactive_if0_define_line_control_resync
int inserted = 0;
#line 2 "header_inactive_if0_define_line_control_resync.c"
#include "h_if0_line.h"
int tail = 3;
