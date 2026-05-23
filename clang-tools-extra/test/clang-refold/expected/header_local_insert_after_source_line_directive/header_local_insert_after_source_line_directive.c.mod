// RUN: %clang-refold-tester-with-lines header_local_insert_after_source_line_directive
#line 80 "virtual_header.c"
int value = 1;
int inserted = 0;
#line 81 "virtual_header.c"
int observed = __LINE__;
const char *file = __FILE__;
int tail = 3;
