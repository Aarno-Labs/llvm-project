// RUN: %clang-refold-tester-with-lines higher_order_two_generated_calls_same_pattern_two_slots
#define APPLY(F, G, X, Y) F(G, X, Y)
#define FWD_PAIR(G, X, Y) G(X) G(Y)
#define STR(x) #x

const char *s = APPLY(FWD_PAIR, STR, alpha, beta);
