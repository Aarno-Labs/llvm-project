// RUN: %clang-refold-tester-with-lines tu_sideband_insert_between_neighbors_preserve_define
#pragma vendor alpha
#define VALUE 1
#pragma vendor beta
#pragma vendor gamma
#line 5 "tu_sideband_insert_between_neighbors_preserve_define.c"
int value = 2;
