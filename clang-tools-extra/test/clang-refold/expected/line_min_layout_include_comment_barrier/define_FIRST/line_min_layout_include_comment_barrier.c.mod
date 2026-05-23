// RUN: %clang-refold-tester-with-lines line_min_layout_include_comment_barrier FIRST
// zero-token header comment that affects -E -P prefix layout
int value = 2;
