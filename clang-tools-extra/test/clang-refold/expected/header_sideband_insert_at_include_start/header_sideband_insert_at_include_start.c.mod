// RUN: %clang-refold-tester-with-lines header_sideband_insert_at_include_start
#pragma vendor inserted
int value = 1;
int tail = 3;
