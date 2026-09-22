// RUN: %clang-refold-tester-with-lines line_control_stringified_filename_utf8_octal_escape
#define FILE_NAME(x) #x
int x =
3
#line 123 FILE_NAME(gap \303\251 c)

;
int y = __LINE__;
const char *f = __FILE__;
