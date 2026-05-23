// RUN: %clang-refold-tester-with-lines include_materialization_with_line_sensitive_file_suffix
int file_value = 2;
#line 3 "include_materialization_with_line_sensitive_file_suffix.c"
const char *tail_file = __FILE__;
