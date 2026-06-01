// RUN: %clang-refold-tester-with-lines line_include_level_builtin_numeric_operand
#line __INCLUDE_LEVEL__ "inclevel_logical.c"
int deleted = 1;
int keep = __LINE__;
const char *file = __FILE__;
