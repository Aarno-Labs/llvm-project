// RUN: %clang-refold-tester-with-lines tu_raw_paste_line_number_resync
#define RAW 4
#define RAW00 910
#define LOC(x) x ## 00 "raw_paste_line_main.c"
#line LOC(RAW)
int inserted = 0;
#line 910 "raw_paste_line_main.c"
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
