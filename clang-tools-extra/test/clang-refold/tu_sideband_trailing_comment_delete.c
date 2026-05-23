// RUN: %clang-refold-tester-with-lines tu_sideband_trailing_comment_delete
#pragma vendor note /* source-only trailing comment */
int value = 1;
