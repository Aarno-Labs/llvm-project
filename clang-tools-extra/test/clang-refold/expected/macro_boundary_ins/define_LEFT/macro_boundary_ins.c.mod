// RUN: %clang-refold-tester macro_boundary_ins LEFT
// RUN: %clang-refold-tester macro_boundary_ins RIGHT
// RUN: %clang-refold-tester macro_boundary_ins BOTH
// RUN: %clang-refold-tester macro_boundary_ins LEFTNOSPACE
// RUN: %clang-refold-tester macro_boundary_ins RIGHTNOSPACE
// RUN: %clang-refold-tester macro_boundary_ins BOTHNOSPACE
#define BAR(X, Y) (X*Y + X)
#define BAZ(X, Y) (Y*X + Y)
#define FOO(X, Y) (BAR(2*X, Y) + BAZ(X, 2*Y))
int x = 5;
int y = 2;
int max = 10;
int z = y - FOO(max, y);
