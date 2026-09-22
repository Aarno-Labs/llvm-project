// RUN: %clang-refold-tester-with-lines-verify-off mixed_owner_include_closure_two_line_gaps_keep_first_filename
//
// The closure consumes two `#line` directives.  The second names no file, so
// it keeps "first.c" from the first, and the suffix reads that file.  The
// resume re-establishing the suffix's state must start from the file in effect
// at the second directive, not from the physical file name.
int x =
1
#line 50 "first.c"
+
#line 123
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;
