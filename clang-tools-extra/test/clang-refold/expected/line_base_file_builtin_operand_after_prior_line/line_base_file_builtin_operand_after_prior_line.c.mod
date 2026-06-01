// RUN: %clang-refold-tester-with-lines line_base_file_builtin_operand_after_prior_line
#line 10 "prior_logical.c"
#line 100 __BASE_FILE__
#line 101 "prior_logical.c"
int keep = __LINE__;
const char *file = __FILE__;
