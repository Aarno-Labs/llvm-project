// RUN: %clang-refold-tester-with-lines header_gnu_named_variadic_mid_resync
#line 1 "headers/h_gnu_named_mid.h"
#define LOC(args...) args
#line LOC(1210 "gnu_named_header.c")
int head = 1;
int inserted = 0;
#line 1211 "gnu_named_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_gnu_named_variadic_mid_resync.c"
int tail = 3;
