// RUN: %clang-refold-tester-with-lines higher_order_selector_alias_target_change
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define SEL STR
#define SEL2 WRAP
#define STR(x) #x
#define WRAP(x) "[" #x "]"

const char *s = APPLY(FWD, SEL, alpha);
