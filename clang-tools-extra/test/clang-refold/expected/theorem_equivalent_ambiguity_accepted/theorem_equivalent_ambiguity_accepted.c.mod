// RUN: %clang-refold-tester theorem_equivalent_ambiguity_accepted
#define FOO(X,Y) ((X + Y) * X)
printf("result: %d\n", FOO(3,5));
