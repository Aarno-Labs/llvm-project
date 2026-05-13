// RUN: %clang-refold-tester header_macro_state_redefinition_shadowing
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#define FOO 5
int y2 = FOO;
#define FOO 50
int z = FOO + 1;
int y = y2;
int result = (y + z) * 500;
