// RUN: %clang-refold-tester-with-lines tu_sideband_insert_before_next_sideband_different_gap
#pragma vendor inserted
#line 2 "tu_sideband_insert_before_next_sideband_different_gap.c"
int first = 1;
#pragma vendor marker
int second = 3;
