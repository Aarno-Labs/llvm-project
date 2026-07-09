// RUN: %clang-refold-tester pragma_directive_edits_on_both_sides
int a = 9;
#pragma message("mid")
int b = 8;
