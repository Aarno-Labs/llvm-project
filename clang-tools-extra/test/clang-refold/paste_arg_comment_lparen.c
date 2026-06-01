// RUN: %clang-refold-tester-with-lines paste_arg_comment_lparen
#define CAT(a, b) a##b
int CAT(fo/* ( */, o) = 1;
