// RUN: %clang-refold-tester-with-lines tu_function_like_line_file_bundle_resync
#define LOC(n, f) n f
#line LOC(730, "bundle_func.c")
int inserted = 0;
#line 730 "bundle_func.c"
int value = __LINE__;
const char *file = __FILE__;
