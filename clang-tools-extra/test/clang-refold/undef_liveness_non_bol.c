// RUN: %clang-refold-tester undef_liveness_non_bol
#define KEEP(x) ((x) + 1)
#define FOO 7

int untouched = KEEP(5);

int before = 1;
#undef FOO
int after = 2;
int use = FOO;
