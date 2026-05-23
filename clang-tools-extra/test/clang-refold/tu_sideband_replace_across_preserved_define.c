// RUN: %clang-refold-tester-with-lines tu_sideband_replace_across_preserved_define
#pragma vendor alpha
#define VALUE 1
#pragma vendor gamma
int value = VALUE;
