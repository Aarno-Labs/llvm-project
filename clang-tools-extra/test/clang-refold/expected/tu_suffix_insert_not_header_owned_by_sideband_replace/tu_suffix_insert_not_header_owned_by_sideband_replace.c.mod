// RUN: %clang-refold-tester-with-lines tu_suffix_insert_not_header_owned_by_sideband_replace
#line 1 "headers/h_suffix_replace.h"
#pragma vendor beta
int value = 1;
int after = 0;
#line 3 "tu_suffix_insert_not_header_owned_by_sideband_replace.c"
int tail = 3;
