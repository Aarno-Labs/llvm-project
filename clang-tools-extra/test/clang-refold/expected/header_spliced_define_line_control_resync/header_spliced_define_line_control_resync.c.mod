// RUN: %clang-refold-tester-with-lines header_spliced_define_line_control_resync
int inserted = 0;
#line 2 "header_spliced_define_line_control_resync.c"
#include "h_spliced_define_line.h"
int tail = 3;
