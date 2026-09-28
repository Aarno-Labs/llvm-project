// RUN: %clang-refold-tester-with-lines-verify-off line_resync_after_comment_before_line_number
//
// A `#line` whose line number follows a block comment that spans a physical
// line break renames the line after the directive to exactly the number.
//
// Clang keys the line note at the number token, which already sits on the
// directive's last physical line, so `int p` is line 100 and `__LINE__` below
// reads 102.  The resync after deleting `int p` must name 101.
#line /* the number
         follows */ 100
int p = 1;
int q = 2;
int r = __LINE__;
