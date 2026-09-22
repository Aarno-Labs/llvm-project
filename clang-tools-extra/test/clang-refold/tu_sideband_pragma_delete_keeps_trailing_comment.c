// RUN: %clang-refold-tester-with-lines tu_sideband_pragma_delete_keeps_trailing_comment
#pragma vendor note /* source-only trailing comment */
int value = 1;
