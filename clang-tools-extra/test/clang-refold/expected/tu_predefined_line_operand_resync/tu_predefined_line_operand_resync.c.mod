#line 1 "tu_predefined_line_operand_resync.c"
// RUN: %clang-refold-tester-with-lines tu_predefined_line_operand_resync
// Keep the directive on physical line 3.
#line __LINE__ "predef_line.c"
int inserted = 0;
#line 3 "predef_line.c"
int value = __LINE__;
const char *file = __FILE__;
