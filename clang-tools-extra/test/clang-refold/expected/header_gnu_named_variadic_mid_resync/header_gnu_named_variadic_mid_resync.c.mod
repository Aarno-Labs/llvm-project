// RUN: %clang-refold-tester-with-lines header_gnu_named_variadic_mid_resync
#define LOC(args...) args
#line LOC(1210 "gnu_named_header.c")
int head = 1;
int inserted = 0;
#line 1211 "gnu_named_header.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
