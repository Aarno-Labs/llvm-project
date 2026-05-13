#line 1 "line_control_stringified_filename_hex_escape.c"
// RUN: %clang-refold-tester-with-lines line_control_stringified_filename_hex_escape
#define FILE_NAME(x) #x
int x =
3
#line 124 "gap A c"
;
int y = __LINE__;
const char *f = __FILE__;
