// RUN: %clang-refold-tester-with-lines header_sideband_insert_at_include_start
#line 1 "headers/h_insert_start.h"
#pragma vendor inserted
int value = 1;
#line 3 "header_sideband_insert_at_include_start.c"
int tail = 3;
