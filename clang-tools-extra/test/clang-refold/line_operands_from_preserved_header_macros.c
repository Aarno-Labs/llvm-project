// RUN: %clang-refold-tester-with-lines line_operands_from_preserved_header_macros
#include "header.h"
#line LOC_LINE LOC_FILE
int deleted = 1;
int keep = __LINE__;
const char *file = __FILE__;
