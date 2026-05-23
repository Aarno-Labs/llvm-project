// RUN: %clang-refold-tester-with-lines line_control_repeated_nested_filename_macro
#define F "gap.c"
#define FILE_NAME F F
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;
const char *f = __FILE__;
