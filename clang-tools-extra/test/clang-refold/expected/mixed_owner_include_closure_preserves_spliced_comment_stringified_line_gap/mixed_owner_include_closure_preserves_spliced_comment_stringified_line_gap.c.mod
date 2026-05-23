// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_spliced_comment_stringified_line_gap
#define FILE_NAME(x) #x
int x =
3
#line 125 "gap c"
;
int y = __LINE__;
const char *f = __FILE__;
