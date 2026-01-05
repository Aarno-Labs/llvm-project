// RUN: %clang-refold-tester multiple_arg_instances4
#define BAR(X, Y) (X*Y + X)
#define BAZ(X, Y) (Y*X + Y)
#define FOO(X, Y) (BAR(2*X, Y) + BAZ(X, 2*Y))
int x = 5;
int y = 2;
int max = 15;
int z = ((2*max*y + 2*max) + (2*y*x + 2*y));
