// RUN: %clang-refold-tester-with-lines higher_order_variadic_forward_to_cat3
// 05_higher_order_variadic_forward_to_cat3
#define APPLYV(F, G, ...) F(G, __VA_ARGS__)
#define FWDV(G, ...) G(__VA_ARGS__)
#define CAT3(a, b, c) a##b##c

int APPLYV(FWDV, CAT3, foo, bar, baz) = 1;
