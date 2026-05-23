// RUN: %clang-refold-tester-with-lines header_raw_paste_filename_mid_resync
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
int tail = 3;
