// RUN: %clang-refold-tester-with-lines header_sideband_only_block_two_to_one
#line 1 "headers/pragma_pair.h"
#pragma vendor gamma
#line 3 "header_sideband_only_block_two_to_one.c"
int value = 2;
