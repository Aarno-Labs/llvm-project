// RUN: %clang-refold-tester-with-lines higher_order_object_alias_callee_chain
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define SEL1 SEL2
#define SEL2 STR
#define STR(x) #x

const char *s = APPLY(FWD, SEL1, beta);
