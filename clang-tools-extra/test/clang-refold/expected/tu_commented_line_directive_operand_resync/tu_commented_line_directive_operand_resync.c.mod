// RUN: %clang-refold-tester-with-lines tu_commented_line_directive_operand_resync
#line 960 /* keep whitespace */ "commented_line_main.c"
int inserted = 0;
#line 960 "commented_line_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
