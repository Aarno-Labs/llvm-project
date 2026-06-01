// RUN: %clang-refold-tester-with-lines line_operands_from_preserved_header_macros
#include "header.h"
#line LOC_LINE LOC_FILE
#line 701 "header_logical.c"
int keep = __LINE__;
const char *file = __FILE__;
