#line 1 "line_control_stringified_filename_numeric_control_escape.c"
// RUN: %clang-refold-tester-with-lines line_control_stringified_filename_numeric_control_escape
#define FILE_NAME(x) #x
int x =
3
#line 124 "gap \n c"
;
int y = __LINE__;
const char *f = __FILE__;
