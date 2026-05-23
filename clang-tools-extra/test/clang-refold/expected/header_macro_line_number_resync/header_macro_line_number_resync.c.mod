// RUN: %clang-refold-tester-with-lines header_macro_line_number_resync
int inserted = 0;
#line 2 "header_macro_line_number_resync.c"
#include "h_macro_line.h"
int tail = 3;
