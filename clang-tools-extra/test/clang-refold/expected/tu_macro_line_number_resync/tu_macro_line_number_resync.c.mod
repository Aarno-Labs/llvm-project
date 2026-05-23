// RUN: %clang-refold-tester-with-lines tu_macro_line_number_resync
#define LINE_NO 200
#define FILE_NAME "logical_main.c"
#line LINE_NO FILE_NAME
int inserted = 0;
#line 200 "logical_main.c"
int value = __LINE__;
