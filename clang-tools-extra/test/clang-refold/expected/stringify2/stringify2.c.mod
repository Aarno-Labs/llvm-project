// RUN: %clang-refold-tester stringify2
#define FOO(X) const char *c = #X
FOO(hello-world);
