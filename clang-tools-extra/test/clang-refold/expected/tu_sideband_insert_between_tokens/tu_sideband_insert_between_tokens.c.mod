// RUN: %clang-refold-tester-with-lines tu_sideband_insert_between_tokens
int before = 1;
#pragma vendor inserted
#line 3 "tu_sideband_insert_between_tokens.c"
int after = 3;
