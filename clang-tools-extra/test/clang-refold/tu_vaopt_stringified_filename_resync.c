// RUN: %clang-refold-tester-with-lines tu_vaopt_stringified_filename_resync
#define LOC(n, name, ...) n __VA_OPT__(#name)
#line LOC(1400, vaopt_stringified_tu.c, present)
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
