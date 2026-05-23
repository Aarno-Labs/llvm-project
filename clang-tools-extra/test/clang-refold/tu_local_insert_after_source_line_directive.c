// RUN: %clang-refold-tester-with-lines tu_local_insert_after_source_line_directive
#line 50 "virtual_tu.c"
int value = 1;
int observed = __LINE__;
const char *file = __FILE__;
