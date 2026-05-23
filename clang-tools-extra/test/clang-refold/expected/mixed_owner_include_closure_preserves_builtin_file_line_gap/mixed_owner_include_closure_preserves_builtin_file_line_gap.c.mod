// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_builtin_file_line_gap
int x =
3
#line 124 "mixed_owner_include_closure_preserves_builtin_file_line_gap.c"
;
int y = __LINE__;
const char *f = __FILE__;
