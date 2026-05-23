// RUN: %clang-refold-tester-with-lines tu_sideband_replace_across_preserved_define
#line 3 "tu_sideband_replace_across_preserved_define.c"
#define VALUE 1
#pragma vendor beta
int value = 2;
