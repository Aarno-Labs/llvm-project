// RUN: %clang-refold-tester-with-lines header_function_like_filename_stringify_mid_resync
int head = 1;
#define STR(x) #x
#line 810 STR(header_func_file.c)
int inserted = 0;
#line 810 "header_func_file.c"
const char *file = __FILE__;
int value = __LINE__;
int tail = 3;
