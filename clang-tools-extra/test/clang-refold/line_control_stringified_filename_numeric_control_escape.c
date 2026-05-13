// RUN: %clang-refold-tester-with-lines line_control_stringified_filename_numeric_control_escape
#define FILE_NAME(x) #x
int x =
1 +
#line 123 FILE_NAME(gap \x0a c)
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;
