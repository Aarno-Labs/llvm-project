// RUN: %clang-refold-tester-with-lines tu_sideband_insert_before_existing_same_gap
#pragma vendor beta
#line 2 "tu_sideband_insert_before_existing_same_gap.c"
#pragma vendor alpha
int value = 2;
