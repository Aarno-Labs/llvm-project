// RUN: %clang-refold-tester-with-lines header_spliced_line_directive_operand_resync
int inserted = 0;
#line 2 "header_spliced_line_directive_operand_resync.c"
#line 1 "headers/h_spliced_line_directive.h"
#line 955 \
"spliced_operand_header.c"
int value = 955;
const char *file = __FILE__;
#line 3 "header_spliced_line_directive_operand_resync.c"
int tail = 3;
