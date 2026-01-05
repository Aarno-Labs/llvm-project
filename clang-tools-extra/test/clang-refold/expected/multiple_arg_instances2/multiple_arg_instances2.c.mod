// RUN: %clang-refold-tester multiple_arg_instances2
#define FOO(X,Y) ((X + Y) * X)
printf("result: %d\n", FOO(3,5));
