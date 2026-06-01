// RUN: %clang-refold-tester-with-lines higher_order_selector_stringify_to_wrap
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define STR(x) #x
#define WRAP(x) "[" #x "]"

const char *s = APPLY(FWD, WRAP, alpha);
