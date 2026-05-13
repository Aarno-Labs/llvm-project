// RUN: %clang-refold-tester header_macro_state_liveness_material_child_include
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#define FOO 7
int before = 10,
#undef FOO
 after = 20;
int use = FOO;
