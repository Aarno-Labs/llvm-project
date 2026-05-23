// RUN: %clang-refold-tester-with-lines header_function_like_filename_stringify_mid_resync
#line 1 "headers/h_func_file_mid.h"
int head = 1;
#define STR(x) #x
#line 810 STR(header_func_file.c)
int inserted = 0;
#line 810 "header_func_file.c"
const char *file = __FILE__;
int value = __LINE__;
#line 3 "header_function_like_filename_stringify_mid_resync.c"
int tail = 3;
