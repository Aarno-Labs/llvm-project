// RUN: %clang-refold-tester-with-lines sideband_duplicate_delete_first_shifted_gap
#pragma vendor note
int before = 1;
#pragma vendor note
int after = 2;
