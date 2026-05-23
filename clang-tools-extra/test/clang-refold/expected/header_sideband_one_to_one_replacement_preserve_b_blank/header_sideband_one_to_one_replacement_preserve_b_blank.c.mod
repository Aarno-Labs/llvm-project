// RUN: %clang-refold-tester-with-lines header_sideband_one_to_one_replacement_preserve_b_blank
#line 1 "headers/h_equal_blank.h"
#pragma vendor beta

#line 2 "headers/h_equal_blank.h"
int value = 1;
#line 3 "header_sideband_one_to_one_replacement_preserve_b_blank.c"
int tail = 3;
