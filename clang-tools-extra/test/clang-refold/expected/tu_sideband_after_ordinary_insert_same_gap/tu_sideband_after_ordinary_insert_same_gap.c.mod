// RUN: %clang-refold-tester-with-lines tu_sideband_after_ordinary_insert_same_gap
int inserted = 0;
#pragma vendor beta
int value = 2;
