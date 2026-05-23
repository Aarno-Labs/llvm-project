// RUN: %clang-refold-tester-with-lines tu_sideband_insert_after_prev_sideband_different_gap
#pragma vendor marker
int first = 1;
#pragma vendor inserted
#line 4 "tu_sideband_insert_after_prev_sideband_different_gap.c"
int second = 3;
