// RUN: %clang-refold-tester-with-lines higher_order_vaopt_forward_to_field
// 06_higher_order_vaopt_forward_to_field
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__)
#define FWD_OPT(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__))
#define FIELD(name, value) #name ":" value

const char *s = APPLY(FWD_OPT, FIELD, beta, "10");
