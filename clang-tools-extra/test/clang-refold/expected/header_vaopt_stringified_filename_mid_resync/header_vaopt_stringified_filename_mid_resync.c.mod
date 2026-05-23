// RUN: %clang-refold-tester-with-lines header_vaopt_stringified_filename_mid_resync
#line 1 "headers/h_vaopt_string_mid.h"
#define LOC(n, name, ...) n __VA_OPT__(#name)
#line LOC(1410, vaopt_stringified_header.c, present)
int head = 1;
int inserted = 0;
#line 1411 "vaopt_stringified_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_vaopt_stringified_filename_mid_resync.c"
int tail = 3;
