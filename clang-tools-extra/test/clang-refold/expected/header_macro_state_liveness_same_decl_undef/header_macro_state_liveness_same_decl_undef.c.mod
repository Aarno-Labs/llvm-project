// RUN: %clang-refold-tester header_macro_state_liveness_same_decl_undef
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#define FOO 7
int before = 10,
#undef FOO
    middle = 20;
int use = FOO;
