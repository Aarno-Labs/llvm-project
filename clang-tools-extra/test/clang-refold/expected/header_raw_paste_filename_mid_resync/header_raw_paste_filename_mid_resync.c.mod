// RUN: %clang-refold-tester-with-lines header_raw_paste_filename_mid_resync
#line 1 "headers/h_raw_paste_file.h"
int head = 1;
#define RAW file
#define file wrong
#define RAW_NAME "raw_paste_file_header.c"
#define wrong_NAME "expanded_paste_file_header.c"
#define LOC(x) 940 x ## _NAME
#line LOC(RAW)
int inserted = 0;
#line 940 "raw_paste_file_header.c"
const char *file_name = __FILE__;
int value = __LINE__;
#line 3 "header_raw_paste_filename_mid_resync.c"
int tail = 3;
