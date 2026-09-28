// RUN: %clang-refold-tester-with-lines-verify-off line_resync_counts_spliced_line_breaks
//
// A newline-drift resync must count every physical line break after a `#line`,
// including the one a backslash-newline splice removes from the token stream.
//
// Clang numbers presumed lines from the physical buffer, so after `#line 100`
// the spliced declaration occupies lines 100 and 101 and `__LINE__` below reads
// 104.  Deleting `int p` shifts the observer up one line, and the resync that
// restores it must name 103 for the line after the deletion.  Counting only
// unspliced newlines named 102, and `__LINE__` expanded to 103.
#line 100
int a = 1 + \
  2;
#line 103 "line_resync_counts_spliced_line_breaks.c"
int q = 2;
int r = __LINE__;
