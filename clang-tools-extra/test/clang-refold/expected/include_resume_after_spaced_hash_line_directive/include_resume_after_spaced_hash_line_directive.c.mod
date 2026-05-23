// RUN: %clang-refold-tester-with-lines include_resume_after_spaced_hash_line_directive
# line 400 "virtual_spaced.c"
#line 1 "headers/h_value.h"
int value = 2;
#line 401 "virtual_spaced.c"
int observed = __LINE__;
const char *file = __FILE__;
