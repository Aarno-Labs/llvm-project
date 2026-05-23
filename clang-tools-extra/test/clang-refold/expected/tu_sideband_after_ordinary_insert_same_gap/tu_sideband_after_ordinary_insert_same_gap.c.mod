// RUN: %clang-refold-tester-with-lines tu_sideband_after_ordinary_insert_same_gap
int inserted = 0;
#pragma vendor beta
#line 2 "tu_sideband_after_ordinary_insert_same_gap.c"
int value = 2;
