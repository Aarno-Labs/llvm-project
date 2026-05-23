// RUN: %clang-refold-tester-with-lines tu_macro_filename_resync
#define LOGICAL_FILE_NAME "logical_file.c"
#line 700 LOGICAL_FILE_NAME
const char *file = __FILE__;
