// RUN: %clang-refold-tester-with-lines sideband_duplicate_delete_first_shifted_gap
#line 3 "sideband_duplicate_delete_first_shifted_gap.c"
int before = 1 + 2;
#pragma vendor note
int after = 3;
