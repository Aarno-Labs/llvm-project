// RUN: %clang-refold-tester-with-lines tu_spliced_line_directive_operand_resync
#line 950 \
"spliced_operand_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
