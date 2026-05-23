// RUN: %clang-refold-tester-with-lines header_variadic_raw_paste_line_number_mid_resync
#line 1 "headers/h_variadic_raw_paste_line.h"
int head = 1;
#define RAW 8
#define RAW0 980
#define LOC(...) __VA_ARGS__ ## 0 "variadic_raw_paste_header.c"
#line LOC(RAW)
int inserted = 0;
#line 980 "variadic_raw_paste_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_variadic_raw_paste_line_number_mid_resync.c"
int tail = 3;
