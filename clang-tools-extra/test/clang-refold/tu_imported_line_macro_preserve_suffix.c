// RUN: %clang-refold-tester-with-lines tu_imported_line_macro_preserve_suffix
#include "imported_line_macro.h"
#line IMPORTED_LINE_LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
