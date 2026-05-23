// RUN: %clang-refold-tester-with-lines bol_undef_liveness_replacement_contains_macro_name
#define M() 10
#undef M
long
M;

int M(void);
int use = M();
