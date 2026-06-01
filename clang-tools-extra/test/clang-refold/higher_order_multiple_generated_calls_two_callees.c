// RUN: %clang-refold-tester-with-lines higher_order_multiple_generated_calls_two_callees
#define APPLY(F, G, H, X) F(G, H, X)
#define FWD_BOTH(G, H, X) G(X) H(X)
#define STR(x) #x
#define WRAP(x) "[" #x "]"

const char *s = APPLY(FWD_BOTH, STR, WRAP, alpha);
