// RUN: %clang-refold-tester-with-lines header_spliced_line_directive_operand_resync
int inserted = 0;
#line 955 \
"spliced_operand_header.c"
int value = 955;
const char *file = __FILE__;
int tail = 3;
