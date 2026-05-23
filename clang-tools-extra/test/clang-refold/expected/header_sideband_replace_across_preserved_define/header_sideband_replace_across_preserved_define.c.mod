// RUN: %clang-refold-tester-with-lines header_sideband_replace_across_preserved_define
#line 2 "headers/h_sideband_replace_across_define.h"
#define VALUE 1
#pragma vendor beta
int value = 2;
#line 3 "header_sideband_replace_across_preserved_define.c"
int tail = 3;
