// RUN: %clang-refold-tester-with-lines header_sideband_insert_after_prev_preserve_define
#line 1 "headers/h_define_after.h"
#pragma vendor alpha
#define VALUE 1
#pragma vendor beta
int value = 2;
#line 3 "header_sideband_insert_after_prev_preserve_define.c"
int tail = 3;
