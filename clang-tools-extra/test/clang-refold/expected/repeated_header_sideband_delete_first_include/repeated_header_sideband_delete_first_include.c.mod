// RUN: %clang-refold-tester-with-lines repeated_header_sideband_delete_first_include
#line 2 "headers/h_repeat_unknown.h"
int repeated = 1;
#line 3 "repeated_header_sideband_delete_first_include.c"
#include "h_repeat_unknown.h"
int tail = 2;
