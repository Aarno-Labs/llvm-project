// RUN: %clang-refold-tester-with-lines paste_arg_comment_rparen
#define CAT(a, b) a##b
int CAT(ba/* ) */, r) = 1;
