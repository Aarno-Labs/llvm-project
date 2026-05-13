#line 1 "mixed_owner_include_closure_preserves_empty_line_macro_gap.c"
// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_empty_line_macro_gap
#define EMPTY
int x =
3
#line 124 "gap.c"
;
int y = __LINE__;
const char *f = __FILE__;
