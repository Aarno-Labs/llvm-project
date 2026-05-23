// RUN: %clang-refold-tester-with-lines tu_function_like_filename_stringify_resync
#define STR(x) #x
#line 620 STR(logical_func_file.c)
int inserted = 0;
#line 620 "logical_func_file.c"
const char *file = __FILE__;
int value = __LINE__;
