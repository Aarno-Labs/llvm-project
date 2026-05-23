// RUN: %clang-refold-tester-with-lines tu_raw_paste_filename_resync
#define RAW file
#define file wrong
#define RAW_NAME "raw_paste_file_main.c"
#define wrong_NAME "expanded_paste_file_main.c"
#define LOC(x) 930 x ## _NAME
#line LOC(RAW)
int inserted = 0;
#line 930 "raw_paste_file_main.c"
const char *file_name = __FILE__;
int value = __LINE__;
int tail = 3;
