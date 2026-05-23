// RUN: %clang-refold-tester-with-lines repeated_header_sideband_only_insert_second
#include "pragma_only2.h"
int gap = 0;
#line 1 "headers/pragma_only2.h"
#pragma vendor alpha
#pragma vendor beta
#line 5 "repeated_header_sideband_only_insert_second.c"
int value = 2;
