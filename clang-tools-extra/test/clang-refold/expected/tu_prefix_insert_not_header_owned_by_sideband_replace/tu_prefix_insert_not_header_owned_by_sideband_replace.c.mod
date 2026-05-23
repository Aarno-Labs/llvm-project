// RUN: %clang-refold-tester-with-lines tu_prefix_insert_not_header_owned_by_sideband_replace
int before = 0;
#line 1 "headers/h_prefix_replace.h"
#pragma vendor beta
int value = 1;
#line 3 "tu_prefix_insert_not_header_owned_by_sideband_replace.c"
int tail = 3;
