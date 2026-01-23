// RUN: %clang-refold-tester stringify3
#define BAR(X,Y) printf("hello: %s, %s\n", X, Y)
#define FOO(X,Y) BAR(#X, #Y)
FOO(billy, corgan);
