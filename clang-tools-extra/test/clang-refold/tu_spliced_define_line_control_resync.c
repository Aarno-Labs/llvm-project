// RUN: %clang-refold-tester-with-lines tu_spliced_define_line_control_resync
#define LOC \
  930 "spliced_define_main.c"
#line LOC
int value = __LINE__;
const char *file = __FILE__;
int tail = 3;
