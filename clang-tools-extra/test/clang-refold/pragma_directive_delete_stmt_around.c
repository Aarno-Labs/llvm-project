// RUN: %clang-refold-tester pragma_directive_delete_stmt_around
int a = 1;
#pragma message("x")
int b = 2;
int c = 3;
