#line 1 "include_resume_after_line_without_filename.c"
// RUN: %clang-refold-tester-with-lines include_resume_after_line_without_filename
#line 200
#line 1 "headers/h_value.h"
int value = 2;
#line 201 "include_resume_after_line_without_filename.c"
int observed = __LINE__;
const char *file = __FILE__;
