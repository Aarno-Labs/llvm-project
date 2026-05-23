// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_builtin_base_file_line_gap
int x =
3
#line 124 "mixed_owner_include_closure_preserves_builtin_base_file_line_gap.c"
;
int y = __LINE__;
const char *f = __FILE__;
const char *b = __BASE_FILE__;
