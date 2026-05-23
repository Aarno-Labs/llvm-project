// RUN: %clang-refold-tester-with-lines header_raw_paste_line_number_mid_resync
#line 1 "headers/h_raw_paste_line.h"
int head = 1;
#define RAW 5
#define RAW00 920
#define LOC(x) x ## 00 "raw_paste_line_header.c"
#line LOC(RAW)
int inserted = 0;
#line 920 "raw_paste_line_header.c"
int value = __LINE__;
const char *file = __FILE__;
#line 3 "header_raw_paste_line_number_mid_resync.c"
int tail = 3;
