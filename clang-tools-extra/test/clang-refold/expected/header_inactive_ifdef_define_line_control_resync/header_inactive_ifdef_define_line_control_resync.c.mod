// RUN: %clang-refold-tester-with-lines header_inactive_ifdef_define_line_control_resync
int inserted = 0;
#line 2 "header_inactive_ifdef_define_line_control_resync.c"
#include "h_ifdef_line.h"
int tail = 3;
