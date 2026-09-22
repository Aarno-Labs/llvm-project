// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_line_gap_with_trailing_tokens
#define FILE_NAME "gap" ".c"
int x =
3
#line 123 FILE_NAME

;
int y = __LINE__;
const char *f = __FILE__;
