// RUN: %clang-refold-tester-with-lines header_sideband_only_block_one_to_two
#line 1 "headers/pragma_one.h"
#pragma vendor gamma
#pragma vendor delta
#line 3 "header_sideband_only_block_one_to_two.c"
int value = 2;
