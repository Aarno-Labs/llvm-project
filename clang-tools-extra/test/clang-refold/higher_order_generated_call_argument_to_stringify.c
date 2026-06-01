// RUN: %clang-refold-tester-with-lines higher_order_generated_call_argument_to_stringify
// 04_higher_order_generated_call_argument_to_stringify
#define APPLY(F, H, G, X) F(H, G, X)
#define FWD_COMPOSE(H, G, X) H(G(X))
#define STR(x) #x
#define ID(x) x

const char *s = APPLY(FWD_COMPOSE, STR, ID, alpha);
