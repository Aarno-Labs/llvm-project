// RUN: %clang-refold-tester-with-lines tu_sideband_insert_after_prev_preserve_define
#pragma vendor alpha
#define VALUE 1
#pragma vendor beta
#line 4 "tu_sideband_insert_after_prev_preserve_define.c"
int value = 2;
