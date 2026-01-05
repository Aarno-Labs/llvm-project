// RUN: %clang-refold-tester multiple_arg_instances1
#define FOO(X,Y) ((X + Y) * X)
printf("result: %d\n", ((3 + 5) * 2));
