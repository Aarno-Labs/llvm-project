// RUN: %clang-refold-tester pragma_directive_edits_on_both_sides
int a = 1;
#pragma message("mid")
int b = 2;
