// RUN: %clang-refold-tester-with-lines include_resume_after_spaced_hash_line_directive
# line 400 "virtual_spaced.c"
int value = 2;
int observed = __LINE__;
const char *file = __FILE__;
