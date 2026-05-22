// RUN: %clang-refold-tester-with-lines sideband_duplicate_delete_first_shifted_gap
int before = 1 + 2;
#pragma vendor note
int after = 3;
