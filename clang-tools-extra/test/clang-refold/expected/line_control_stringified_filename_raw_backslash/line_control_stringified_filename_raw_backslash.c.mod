// RUN: %clang-refold-tester-with-lines line_control_stringified_filename_raw_backslash
#define FILE_NAME(x) #x
int x =
3
#line 123 FILE_NAME(gap \ c)

;
int y = __LINE__;
const char *f = __FILE__;
