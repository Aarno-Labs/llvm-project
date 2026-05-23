// RUN: %clang-refold-tester-with-lines repeated_header_sideband_replace_second_include
#include "replace_note.h"
int gap = 0;
#line 1 "headers/replace_note.h"
#pragma vendor replace_note_changed
int z = 1;
#line 5 "repeated_header_sideband_replace_second_include.c"
int tail = 2;
