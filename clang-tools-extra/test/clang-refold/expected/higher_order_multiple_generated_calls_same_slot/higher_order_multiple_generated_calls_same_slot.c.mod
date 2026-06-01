// RUN: %clang-refold-tester-with-lines higher_order_multiple_generated_calls_same_slot
#define APPLY(F, G, X) F(G, X)
#define FWD_TWICE(G, X) G(X) G(X)
#define STR(x) #x

const char *s = APPLY(FWD_TWICE, STR, beta);
