// RUN: %clang-refold-tester-with-lines header_function_like_line_number_mid_resync
#line 1 "headers/h_func_line_mid.h"
int head = 1;
#define HLINE(x) x
#define HFILE "logical_header_func.c"
#line HLINE(510) HFILE
int inserted = 0;
#line 510 "logical_header_func.c"
int value = __LINE__;
#line 3 "header_function_like_line_number_mid_resync.c"
int tail = 3;
