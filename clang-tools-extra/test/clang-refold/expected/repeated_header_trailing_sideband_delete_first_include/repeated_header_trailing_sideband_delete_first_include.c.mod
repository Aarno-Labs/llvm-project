// RUN: %clang-refold-tester-with-lines repeated_header_trailing_sideband_delete_first_include
#line 1 "headers/trailing_note.h"
int v = 1;
#line 3 "repeated_header_trailing_sideband_delete_first_include.c"
int gap = 0;
#include "trailing_note.h"
int tail = 2;
