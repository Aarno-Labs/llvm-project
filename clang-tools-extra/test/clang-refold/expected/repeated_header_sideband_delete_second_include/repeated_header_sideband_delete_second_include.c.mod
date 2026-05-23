// RUN: %clang-refold-tester-with-lines repeated_header_sideband_delete_second_include
#include "repeated_note.h"
int gap = 0;
#line 2 "headers/repeated_note.h"
int v = 1;
#line 5 "repeated_header_sideband_delete_second_include.c"
int tail = 2;
