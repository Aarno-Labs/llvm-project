// RUN: %clang-refold-tester-with-lines higher_order_stringify_and_param_mix
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD2(G, A, B) G(A, B)
#define FIELD(name, value) #name ":" value

const char *s = APPLY(FWD2, FIELD, beta, "20");
