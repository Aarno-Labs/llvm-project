// RUN: %clang-refold-tester-with-lines tu_macro_line_file_bundle_resync
#define LOGICAL_LOCATION 800 "bundle_file.c"
#line LOGICAL_LOCATION
int inserted = 0;
#line 800 "bundle_file.c"
int line_value = __LINE__;
const char *file_value = __FILE__;
