// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_builtin_file_line_gap
int x =
1 +
#line 123 __FILE__
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;
