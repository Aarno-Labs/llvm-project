// RUN: %clang-refold-tester-with-lines header_sideband_replace_across_preserved_define
#define VALUE 1
#pragma vendor beta
int value = 2;
int tail = 3;
