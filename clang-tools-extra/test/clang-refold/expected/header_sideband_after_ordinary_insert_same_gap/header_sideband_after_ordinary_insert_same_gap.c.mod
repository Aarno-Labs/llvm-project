// RUN: %clang-refold-tester-with-lines header_sideband_after_ordinary_insert_same_gap
int inserted = 0;
#pragma vendor beta
int value = 2;
int tail = 3;
