// RUN: %clang-refold-tester-with-lines header_sideband_line_continuation_replace
#line 1 "headers/h_cont_pragma.h"
#pragma vendor header_multi new
int value = 1;
#line 3 "header_sideband_line_continuation_replace.c"
int tail = 3;
