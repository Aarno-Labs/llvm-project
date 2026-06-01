// RUN: %clang-refold-tester-with-lines higher_order_stringify_actual
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define STR(x) #x

const char *s = APPLY(FWD, STR, beta);
