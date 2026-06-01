// RUN: %clang-refold-tester-with-lines two_level_higher_order_stringify
#define APPLY(F, G, H, X) F(G, H, X)
#define OUTER(G, H, X) G(H, X)
#define INNER(H, X) H(X)
#define STR(x) #x
#define ID(x) x

const char *s = APPLY(OUTER, INNER, STR, ID(beta));
