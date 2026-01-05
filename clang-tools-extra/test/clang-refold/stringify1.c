// RUN: %clang-refold-tester stringify1
#define FOO(X) const char *c = #X
FOO(hello);
