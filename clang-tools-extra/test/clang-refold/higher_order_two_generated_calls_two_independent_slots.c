// RUN: %clang-refold-tester-with-lines higher_order_two_generated_calls_two_independent_slots
#define APPLY(F, G, H, X, Y) F(G, H, X, Y)
#define FWD_BOTH(G, H, X, Y) G(X) H(Y)
#define STR(x) #x
#define WRAP(x) "[" #x "]"

const char *s = APPLY(FWD_BOTH, STR, WRAP, alpha, beta);
