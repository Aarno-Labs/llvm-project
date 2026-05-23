// RUN: %clang-refold-tester-with-lines header_sideband_only_insert_after_existing
#line 1 "./headers/pragma_only.h"
#pragma vendor alpha
#pragma vendor beta
#line 3 "main.c"
int value = 2;
