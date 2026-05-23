// RUN: %clang-refold-tester-with-lines tu_function_like_line_number_resync
#define LINE_ID(x) x
#define LOG_FILE "logical_func_line.c"
#line LINE_ID(410) LOG_FILE
int inserted = 0;
#line 410 "logical_func_line.c"
int value = __LINE__;
