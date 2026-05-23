// RUN: %clang-refold-tester-with-lines include_resume_after_indented_line_directive
  #line 300 "virtual_indented.c"
int value = 2;
int observed = __LINE__;
const char *file = __FILE__;
