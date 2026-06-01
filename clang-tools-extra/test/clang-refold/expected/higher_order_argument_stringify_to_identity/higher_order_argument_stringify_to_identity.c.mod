// RUN: %clang-refold-tester-with-lines higher_order_argument_stringify_to_identity
#define APPLY(F, G, X) F(G, X)
#define FWD_QUOTE(G, X) G(#X)
#define ID(x) x

const char *s = APPLY(FWD_QUOTE, ID, beta);
