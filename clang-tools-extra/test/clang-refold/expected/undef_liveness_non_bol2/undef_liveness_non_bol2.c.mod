// RUN: %clang-refold-tester undef_liveness_non_bol2
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#define FOO 7

int before = 10,
#undef FOO
 after = 20;
int use = FOO;
