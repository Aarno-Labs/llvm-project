// RUN: %clang-refold-tester-with-lines tu_sideband_insert_after_prev_preserve_define
#pragma vendor alpha
#define VALUE 1
int value = VALUE;
