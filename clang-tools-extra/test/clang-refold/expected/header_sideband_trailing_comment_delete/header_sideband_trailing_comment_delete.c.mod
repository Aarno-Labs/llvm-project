// RUN: %clang-refold-tester-with-lines header_sideband_trailing_comment_delete
#line 2 "headers/h_comment_pragma.h"
int value = 1;
#line 3 "header_sideband_trailing_comment_delete.c"
int tail = 3;
