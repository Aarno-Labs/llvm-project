// RUN: %clang-refold-tester-with-lines header_sideband_insert_between_neighbors_preserve_define
#pragma vendor alpha
#define VALUE 1
#pragma vendor beta
#pragma vendor gamma
int value = 2;
int tail = 3;
