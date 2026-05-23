#line 1 "tu_local_insert_after_source_line_directive.c"
// RUN: %clang-refold-tester-with-lines tu_local_insert_after_source_line_directive
#line 50 "virtual_tu.c"
int value = 1;
int inserted = 0;
#line 51 "virtual_tu.c"
int observed = __LINE__;
const char *file = __FILE__;
