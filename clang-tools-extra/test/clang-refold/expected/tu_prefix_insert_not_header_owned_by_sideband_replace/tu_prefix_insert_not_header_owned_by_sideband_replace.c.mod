// RUN: %clang-refold-tester-with-lines tu_prefix_insert_not_header_owned_by_sideband_replace
int before = 0;
#pragma vendor beta
int value = 1;
int tail = 3;
