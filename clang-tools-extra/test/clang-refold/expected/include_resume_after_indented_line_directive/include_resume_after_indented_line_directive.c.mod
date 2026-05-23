// RUN: %clang-refold-tester-with-lines include_resume_after_indented_line_directive
  #line 300 "virtual_indented.c"
#line 1 "headers/h_value.h"
int value = 2;
#line 301 "virtual_indented.c"
int observed = __LINE__;
const char *file = __FILE__;
