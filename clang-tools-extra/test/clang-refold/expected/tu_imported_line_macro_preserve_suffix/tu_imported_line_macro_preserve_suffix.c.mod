#line 1 "tu_imported_line_macro_preserve_suffix.c"
// RUN: %clang-refold-tester-with-lines tu_imported_line_macro_preserve_suffix
#include "imported_line_macro.h"
#line IMPORTED_LINE_LOC
int inserted = 0;
int value = 811;
const char *file = __FILE__;
int tail = 3;
