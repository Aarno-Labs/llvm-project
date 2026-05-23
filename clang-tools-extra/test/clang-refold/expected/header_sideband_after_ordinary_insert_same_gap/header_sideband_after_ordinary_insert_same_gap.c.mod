// RUN: %clang-refold-tester-with-lines header_sideband_after_ordinary_insert_same_gap
#line 1 "headers/h_insert_order.h"
int inserted = 0;
#pragma vendor beta
#line 1 "headers/h_insert_order.h"
int value = 2;
#line 3 "header_sideband_after_ordinary_insert_same_gap.c"
int tail = 3;
