// RUN: %clang-refold-tester-with-lines header_commented_line_directive_operand_resync
int inserted = 0;
#line 2 "header_commented_line_directive_operand_resync.c"
#include "h_commented_line.h"
int tail = 3;
