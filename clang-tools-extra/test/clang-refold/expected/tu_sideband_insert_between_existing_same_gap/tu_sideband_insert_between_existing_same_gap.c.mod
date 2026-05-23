// RUN: %clang-refold-tester-with-lines tu_sideband_insert_between_existing_same_gap
#pragma vendor alpha
#pragma vendor beta
#line 3 "tu_sideband_insert_between_existing_same_gap.c"
#pragma vendor gamma
int value = 2;
