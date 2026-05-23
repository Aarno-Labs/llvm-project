// RUN: %clang-refold-tester-with-lines header_vaopt_stringified_filename_mid_resync
#define LOC(n, name, ...) n __VA_OPT__(#name)
#line LOC(1410, vaopt_stringified_header.c, present)
int head = 1;
int inserted = 0;
#line 1411 "vaopt_stringified_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
