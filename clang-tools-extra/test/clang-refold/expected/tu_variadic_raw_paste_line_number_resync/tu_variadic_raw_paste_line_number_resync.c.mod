// RUN: %clang-refold-tester-with-lines tu_variadic_raw_paste_line_number_resync
#define RAW 7
#define RAW0 970
#define LOC(...) __VA_ARGS__ ## 0 "variadic_raw_paste_main.c"
#line LOC(RAW)
int inserted = 0;
#line 970 "variadic_raw_paste_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
